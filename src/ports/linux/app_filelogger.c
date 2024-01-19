#include "app_filelogger.h"

#include "app_data.h"
#include "app_gsdml.h"
#include "app_log.h"
#include "logger_common.h"

#include "osal.h"
#include "osal_log.h" /* For LOG_LEVEL */
#include "pnal.h"
#include "pnal_filetools.h"
#include <pnet_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <limits.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>

static os_mutex_t *mutex;
static os_sem_t *finishDataSemaphore;
static entry_buffer_t entries;
static bool bigendian = true;

static void log_thread_main(void * arg);
static void archive_thread_main(void *arg);

int addLogEntry(
	DTL_data_t *timestamp,
	uint8_t *word_data,
	uint8_t word_count)
{
	if(mutex == NULL) {
		/* entry buffer is not yet initialised */
		initialiseLoggerThread(&entries);
	}
	
	uint16_t year;
	uint32_t nano;
	
	if(bigendian) {
		year = CC_TO_BE16(timestamp->year);
		nano = CC_TO_BE32(timestamp->nanosecond);
	}
	else {
		year = CC_TO_LE16(timestamp->year);
		nano = CC_TO_LE32(timestamp->nanosecond);
	}
	
	if(timestamp->year == 0) {
		APP_LOG_WARNING("Entry looks uninitialised (var1=%x%x), ignoring\n", word_data[0], word_data[1]);
		return -1;
	}
	
	/*
	OSAL does not wrap pthread_mutex_timedlock
	This is running on the important thread so we'd like to fail fairly quickly
	Could write a wrapper, or just call directly w/ cast as that's all OSAL does
	or just ensure the logging thread doesn't hold it for too long...
	*/
	os_mutex_lock(mutex);
	
	static unsigned int drop_count = 0;
	static unsigned int next_logged_drop = 2;
	
	if((entries.end + ENTRY_SIZE)%ENTRY_BUFFER_SIZE == entries.start) {
		/*
		"no more" room
		strictly speaking one more will fit,
		but will make the buffer look empty (start = end)
		*/
		os_mutex_unlock(mutex);
		++drop_count;
		if(drop_count >= next_logged_drop) {
			APP_LOG_WARNING("Data buffer full - dropped %u entries!\n", drop_count);
			next_logged_drop *= 5;
		}
		return -1;
	}
	
	if(drop_count != 0) {
		APP_LOG_WARNING("[%2d:%02d] Recovered after \e[31m%u\e[0m dropped.\n", timestamp->hour, timestamp->minute, drop_count);
	
		drop_count = 0;
		next_logged_drop = 2;
	}
	
	size_t end = entries.end;
	
	memcpy(entries.buffer+end, &year, 2);
	entries.buffer[end+2] = timestamp->month;
	entries.buffer[end+3] = timestamp->day;
	entries.buffer[end+4] = timestamp->weekday;
	entries.buffer[end+5] = timestamp->hour;
	entries.buffer[end+6] = timestamp->minute;
	entries.buffer[end+7] = timestamp->second;
	memcpy(entries.buffer+end+8, &nano, 4);
	
	memcpy(entries.buffer+end+12, word_data, 2*word_count);
	
	entries.end = (entries.end + ENTRY_SIZE) % ENTRY_BUFFER_SIZE;
	
	os_mutex_unlock(mutex);
	
	return 0;
}

int initialiseLoggerThread(entry_buffer_t *entries)
{
	/*
	os_timer_create will start a thread to run something at an interval
	*/
	mutex = os_mutex_create();
	
	os_thread_create(
		"logger_thread",
		LOG_THREAD_PRIORITY,
		LOG_THREAD_STACKSIZE,
		log_thread_main,
		(void *) entries
	);
	
	return 0;
}

void log_thread_main(void * arg)
{
	entry_buffer_t * entries = (entry_buffer_t *)arg;
	
	log_file_t current_log = {.fd = -1 };
	DTL_data_t curr_log_start = {0};
	
	APP_LOG_DEBUG("\e[92mLogging thread active\e[0m\n");
	
	while(true) {
		os_mutex_lock(mutex);
		
		/* make sure this loop is not unnecesarily slow (e.g. waits on I/O) */
		while(entries->start != entries->end) {
			if(entries->start % ENTRY_SIZE != 0) {
				APP_LOG_ERROR("Log buffer does not start on an entry! %u, offset %u\n",
					entries->start, entries->start % ENTRY_SIZE);
				/* this doesn't happen anyway but repeating an entry (with timestamp) can't hurt */
				entries->start = (entries->start / ENTRY_SIZE) * ENTRY_SIZE;
			}
			DTL_data_t entry_ts;
			uint8_t *entry =  entries->buffer + entries->start;
			
			/* investigate the timestamp */
			memcpy(&entry_ts.year, entry, 2);
			entry_ts.year   = (bigendian) ? CC_FROM_BE16(entry_ts.year) : CC_FROM_LE16(entry_ts.year);
			entry_ts.month  = entry[2];
			entry_ts.day    = entry[3];
			entry_ts.hour   = entry[5];
			entry_ts.minute = entry[6];
			
			/*
			Does this entry belong to the current log?
			"current log" starts at 0000-00-00 when none have been started
			If not, wrap it up and start a new one
			*/
			if(!DTLs_for_same_log(&curr_log_start, &entry_ts)) {
				/* release the lock, so we can do IO in peace */
				os_mutex_unlock(mutex);
				
				if(current_log.fd >= 0) {
					finishLogFile(&current_log, true);
					
					if(    entry_ts.day   != curr_log_start.day
						|| entry_ts.month != curr_log_start.month
						|| entry_ts.year  != curr_log_start.year
					) {
						finishLogGroup(&curr_log_start);
					}
				}
				
				curr_log_start = entry_ts;
				int ret = startLogFile(&current_log, &curr_log_start);
				while(ret == -1) {
					APP_LOG_WARNING("Failed to start log, retrying\n");
					os_usleep(500);
					ret = startLogFile(&current_log, &curr_log_start);
				}
				
				/* ready to process again */
				os_mutex_lock(mutex);
			}
			
			if(current_log.fd == -1) {
				APP_LOG_ERROR("No file?\n");
				break;
			}
			
			if(current_log.buf_end + (ENTRY_SIZE + 1) >= FILE_BUFFER_SIZE) {
				int remaining = ((entries->end - entries->start) % ENTRY_BUFFER_SIZE) / ENTRY_SIZE;
				/* Not losing anything yet, but the buffer needs something cleared before copying any more */
				APP_LOG_WARNING("File buffer running low (\e[33m%d\e[0m/%d), leaving \e[93m%d\e[0m/%d entries\n",
					current_log.buf_end, FILE_BUFFER_SIZE, remaining, ENTRY_BUFFER_SIZE / ENTRY_SIZE);
				break;
			}
			
			/* file format includes 0 before each entry */
			current_log.buffer[current_log.buf_end] = 0;
			memcpy(current_log.buffer +current_log.buf_end+1, entry, ENTRY_SIZE);
			current_log.buf_end += (ENTRY_SIZE + 1);

			entries->start = (entries->start + ENTRY_SIZE) % ENTRY_BUFFER_SIZE;
		}
		
		os_mutex_unlock(mutex);
		
		/* write log if buffer is long enough */
		if(current_log.fd != -1) {
			size_t start = 0;
			
			while(current_log.buf_end - start >= FILE_MIN_WRITE) {
				ssize_t written = write(current_log.fd, current_log.buffer + start, current_log.buf_end - start);
				if(written == -1) {
					if(errno == EDQUOT || errno == ENOSPC) {
						APP_LOG_WARNING("Write failed, clearing space...\n");
						deleteOldest();
					}
					continue;
				}
				start += written;
			}
				
			memmove(current_log.buffer, current_log.buffer + start, current_log.buf_end - start);
			
			current_log.buf_end -= start;
		}
		
		/*
		This really needs to wait on an entry in the entry buffer,
		clearing it as soon as there's something to use and CPU time to do it.
		The main thread has priority so it will always be allowed to add entries
		so long as we do not hold the mutex.
		Lock-free queue (using atomics) could be good.
		*/
		os_usleep(2000);
	}
}

bool DTLs_for_same_log(DTL_data_t *ts_1, DTL_data_t *ts_2)
{
	if(    ts_1->minute/10 == ts_2->minute/10
		&& ts_1->hour == ts_2->hour
		&& ts_1->day == ts_2->day
		&& ts_1->month == ts_2->month
		&& ts_1->year == ts_2->year
	) {
		return true;
	}
	else {
		return false;
	}
}

int getLogDir() {
	static int logdir_fd = -1;
	int ret;
	
	if(logdir_fd != -1)
		return logdir_fd;
	
	/* FHS says /var/opt is required to exist, so assume it does */
	
	ret = mkdir("/var/opt/pnlogger", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
	if(ret == -1 && errno != EEXIST) {
		APP_LOG_ERROR("Failed to create /var/opt/pnlogger\n");
		return -1;
	}
	
	ret = mkdir("/var/opt/pnlogger/data", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
	if(ret == -1 && errno != EEXIST) {
		APP_LOG_ERROR("Failed to create /var/opt/pnlogger/data\n");
		return -1;
	}
	
	/* Sure would be nice if O_CREAT | O_DIRECTORY did the logical thing! */
	
	ret = open("/var/opt/pnlogger/data", O_DIRECTORY);
	if(ret == -1) {
		APP_LOG_ERROR("Failed to open /var/opt/pnlogger/data\n");
		return -1;
	}
	
	logdir_fd = ret;
	
	return logdir_fd;
}

DIR *openLogDir() {
	int logdir_fd = getLogDir();
	if(logdir_fd == -1)
		return NULL;
	
	int fd = openat(logdir_fd, ".", O_DIRECTORY);
	if(fd == -1)
		return NULL;
	
	DIR *logdir = fdopendir(fd);
	if(logdir == NULL)
		return NULL;
	
	return logdir;
}

int startLogFile(log_file_t *log_file, DTL_data_t *timeframe)
{
	/* reusing log_file because the buffers are large */
	log_file->buf_end = 0;
	app_read_log_parameters(log_file->log_id);
	
	char date[16];
	sprintf(date, "%4d%02d%02d", timeframe->year, timeframe->month, timeframe->day);
	
	int logdir_fd = getLogDir();
	if(logdir_fd == -1)
		return -1;
	
	int ret = mkdirat(logdir_fd, date, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
	if(ret == -1 && errno != EEXIST) {
		APP_LOG_ERROR("Failed to create %s\n", date);
		return -1;
	}
	
	/* O_PATH requires _GNU_SOURCE */
	int dirfd = openat(logdir_fd, date, O_DIRECTORY | O_CLOEXEC);
	if(dirfd == -1) {
		APP_LOG_ERROR("Failed to open %s\n", date);
		return -1;
	}
	
	char fname[16];
	sprintf(fname, "%02d-%02d.bin",
		timeframe->hour, 10*(timeframe->minute/10)
	);
	
	int fd = openat(
		dirfd, fname,
		O_WRONLY | O_APPEND | O_CREAT | O_EXCL,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH /* owner RW, others R */
	);
	
	for(int i=2; fd < 0 && errno == EEXIST && i<=9; i++) {
		sprintf(fname, "%02d-%02d_%d.bin",
			timeframe->hour, 10*(timeframe->minute/10), i
		);
		
		fd = openat(
			dirfd, fname,
			O_WRONLY | O_APPEND | O_CREAT | O_EXCL,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH /* owner RW, others R */
		);
	}
	
	if(close(dirfd) == -1) {
		APP_LOG_WARNING("Failed to close %s\n", date);
	}
	
	if(fd < 0) {
		APP_LOG_ERROR("Could not start a log for %s/%02d-%02d\n", date, timeframe->hour, 10*(timeframe->minute/10));
		return -1;
	}
	
	log_file->fd = fd;
	log_file->bigendian = true;
	
	ret = writeLogHeader(log_file);
	if(ret == -1) {
		return -1;
	}
	
	APP_LOG_INFO("Started %s/%s\n", date, fname);
	
	return 0;
}

int writeLogHeader(log_file_t *log_file)
{
	unsigned int header_size = 4+3+1+APP_GSDML_INSTALLATIONID_LENGTH+1;
	uint8_t header[header_size];
	
	/* Assert the format of the file */
	header[0] = 0x61;
	header[1] = 0x0B;
	header[2] = 0xE7;
	header[3] = 0xEC;
	
	/* endianness */
	if(log_file->bigendian) {
		header[4] = 0x50;
		header[5] = 0x4E;
		header[6] = 0x4C;
	}
	else {
		header[4] = 0x4C;
		header[5] = 0x4E;
		header[6] = 0x50;
	}
	
	/* version */
	header[7] = 0;
	
	/* ID */
	memcpy(header+8, log_file->log_id, APP_GSDML_INSTALLATIONID_LENGTH);
	
	/* word count */
	/* digital size is bytes and words are 2 */
	header[8+APP_GSDML_INSTALLATIONID_LENGTH] = APP_GSDML_VAR64_DATA_DIGITAL_SIZE/2;
	
	/* all done, write it */
	ssize_t written = write(log_file->fd, header, header_size);
	if(written == -1) {
		if(errno == EDQUOT || errno == ENOSPC) {
			APP_LOG_WARNING("Write failed, clearing space...\n");
			deleteOldest();
		}
		written = 0;
	}
	
	if(written < header_size) {
		APP_LOG_WARNING(
			"Header incomplete; wrote %d/%d bytes\n",
			written, header_size
		);
		
		/* keep the rest for later */
		size_t remnant = header_size - written;
		memcpy(log_file->buffer, header, remnant);
		log_file->buf_end = remnant;
	}
	
	while(fsync(log_file->fd) == -1) {
		if(errno == EBADF) {
			return -1;
		}
		else if(errno == ENOSPC || errno == EDQUOT) {
			APP_LOG_WARNING("File sync failed, clearing space...\n");
			deleteOldest();
		}
	}
	
	return 0;
}

int finishLogFile(log_file_t *log_file, bool flush)
{
	while(log_file->buf_end >= FILE_BUFFER_SIZE) {
		/* make sure there's room for one more byte! */
		ssize_t written = write(log_file->fd, log_file->buffer, FILE_MIN_WRITE);
		if(written == -1) {
			if(errno == EDQUOT || errno == ENOSPC) {
				APP_LOG_WARNING("Write failed, clearing space...\n");
				deleteOldest();
			}
			continue;
		}
		
		memmove(log_file->buffer, log_file->buffer+written, log_file->buf_end - written);
		
		log_file->buf_end -= written;
	}
	
	log_file->buffer[log_file->buf_end] = 255;
	log_file->buf_end += 1;
	
	/* finish it off */
	size_t start = 0;
	while(start < log_file->buf_end) {
		ssize_t written = write(log_file->fd, log_file->buffer + start, log_file->buf_end - start);
		if(written == -1) {
			if(errno == EDQUOT || errno == ENOSPC) {
				APP_LOG_WARNING("Write failed, clearing space...\n");
				deleteOldest();
			}
			continue;
		}
		start += written;
	}
	
	/* close does not flush, so this does make a difference */
	if(flush) {
		int ret = fsync(log_file->fd);
		while(ret == -1) {
			if(errno == EDQUOT || errno == ENOSPC) {
				APP_LOG_WARNING("File sync failed, clearing space...\n");
				deleteOldest();
			}
			else {
				return -1;
			}
			ret = fsync(log_file->fd);
		}
	}
	
	int ret = close(log_file->fd);
	if(ret == -1) {
		return -1;
	}
	
	APP_LOG_INFO("Saved successfully.\n");
	
	log_file->fd = -1;
	
	return 0;
}

int finishLogGroup(DTL_data_t *timeframe)
{
	finishDataSemaphore = os_sem_create(0);
	
	os_thread_create(
		"log_archive_thread",
		ARCHIVE_PRIORITY,
		4096,
		archive_thread_main,
		(void *) timeframe
	);
	
	/* make sure it knows what to work on, before returning and timeframe is potentially altered */
	os_sem_wait(finishDataSemaphore, OS_WAIT_FOREVER);
	
	os_sem_destroy(finishDataSemaphore);
	
	return 0;
}

void archive_thread_main(void *arg)
{
	
	DTL_data_t *timeframe = (DTL_data_t *) arg;
	uint16_t year = timeframe->year;
	uint8_t month = timeframe->month;
	uint8_t day   = timeframe->day;
	
	/* got the data, let the logging thread continue */
	os_sem_signal(finishDataSemaphore);
	
	int logdir_fd = getLogDir();
	if(logdir_fd == -1)
		return;
	int ret;
	
	/* delete old archives if not much space is available */
	struct statvfs statbuf;
	ret = fstatvfs(logdir_fd, &statbuf);
	while(ret == 0 && statbuf.f_bfree * 100 / statbuf.f_blocks < FREE_SPACE_PERCENT) {
		APP_LOG_INFO("\e[33m%lu/%lu\e[0m blocks available, clearing space...\n", statbuf.f_bfree, statbuf.f_blocks);
		deleteOldest();
		
		ret = fstatvfs(logdir_fd, &statbuf);
	}
	
	/* set scheduling policy to normal */
	APP_LOG_DEBUG("Setting SCHED_OTHER\n");
	struct sched_param schedparam = {0};
	ret = pthread_setschedparam(pthread_self(), SCHED_OTHER, &schedparam);
	if(ret != 0) {
		APP_LOG_WARNING("Archiving: \e[33mCould not set scheduling policy\e[0m\n");
	}
	
	DIR *logdir = openLogDir();
	if(logdir == NULL)
		return;
	
	struct dirent *entry;
	
	for(entry = readdir(logdir); entry != NULL; entry = readdir(logdir)) {
		if(entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN)
			continue;
		   
		unsigned short int _year, _month, _day;
		char end;
		
		int matches = sscanf(entry->d_name, "%4hu%2hu%2hu%c", &_year, &_month, &_day, &end);
		
		if(matches != 3)
			continue;
		
		if(_year > year)
			continue;
		else if(_year == year) {
			if(_month > month)
				continue;
			else if(_month == month) {
				if(_day > day)
					continue;
			}
		}

		compressDirectory(entry->d_name);
	}
	
	/* also closes fd */
	if(closedir(logdir) == -1)
		return;
	
}

int compressDirectory(char *directory) {
	pid_t child_pid;
	int status;
	char archive[24];
	
	sprintf(archive, "%s.tgz", directory);
	
	int logdir_fd = getLogDir();
	if(logdir_fd == -1)
		return -1;
	
	child_pid = fork();
	if(child_pid == 0) {
		/* this is the child */
		
		/* reduce the priority some more */
		nice(10);
		
		if(fchdir(logdir_fd) == -1) {
			APP_LOG_ERROR("Archiving: could not set working directory\n");
			exit(EXIT_FAILURE);
		}
		
		/* xz might yield better ratio,
		   although this is not text and xz is substantially slower */
		char *argv[] = { "tar", "-czf", archive, directory, NULL };
		
		/* p searches so we don't have to */
		execvp("tar", argv);
		
		/* exec didn't replace us... don't want this process hanging around */
		/* Goodbye, world! */
		exit(EXIT_FAILURE);
	}
	else if(child_pid == -1) {
		APP_LOG_ERROR("Archiving: \e[31mFailed to instantiate for %s\e[0m\n", directory);
		return -1;
	}
	
	APP_LOG_INFO("Archiving %s...\n", directory);
	
	waitpid(child_pid, &status, 0);
	
	if(WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		APP_LOG_ERROR("Archiving: \e[31mFailed to archive %s\e[0m\n", archive);
		/* os_thread_create doesn't want us to return a value */
		return -1;
	}
	
	/* delete the now-compressed directory */
	int dir_fd = openat(logdir_fd, directory, O_DIRECTORY);
	if(dir_fd == -1) {
		APP_LOG_ERROR("Archiving: Failed to open %s\n", directory);
		return -1;
	}
	
	DIR *loggroup_dirp = fdopendir(dir_fd);
	if(loggroup_dirp == NULL) {
		APP_LOG_ERROR("Archiving: Failed to open %s\n", directory);
		return -1;
	}
		
	struct dirent *entry;
	
	for(entry = readdir(loggroup_dirp); entry != NULL; entry = readdir(loggroup_dirp)) {
		if(entry->d_name[0] == '.'
		   && (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
			/* I'm so glad these appear in every directory listing */
			continue;
		}
		
		/* use unlinkat because "rmdirat" does not exist */
		if(unlinkat(dir_fd, entry->d_name, 0) == -1) {
			APP_LOG_WARNING("Archiving: \e[31mFailed to delete %s\e[0m\n", entry->d_name);
			/* keep going anyway */
		}
	}
	
	if(closedir(loggroup_dirp) == -1)
		return -1;
	
	if(unlinkat(logdir_fd, directory, AT_REMOVEDIR) == -1) {
		APP_LOG_WARNING("Archiving: \e[31mFailed to delete %s\e[0m\n", directory);
		return -1;
	}
	
	APP_LOG_INFO("Archiving: \e[32mArchived %s as \e[92m%s\e[0m\n", directory, archive);
	
	return 0;
}

int deleteOldest()
{
	DIR *dirp = openLogDir();
	if(dirp == NULL) {
		return -1;
	}
	
	/* date of oldest */
	unsigned short int year = 9999, month = 99, day = 99;
	
	struct dirent *entry;
	
	for(entry = readdir(dirp); entry != NULL; entry = readdir(dirp)) {
		/* components are fixed-length and zero-padded
		   - strcmp could be appropriate? */
		   
		if(entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN)
			continue;
		   
		unsigned short int _year, _month, _day;
		char end;
		
		int matches = sscanf(entry->d_name, "%4hu%2hu%2hu.tgz%c", &_year, &_month, &_day, &end);
		
		if(matches != 3)
			continue;
		
		if(_year > year)
			continue;
		else if(_year < year) {
			year = _year;
			month = _month;
			day = _day;
			continue;
		}
			
		if(_month > month)
			continue;
		else if(_month < month) {
			year = _year;
			month = _month;
			day = _day;
			continue;
		}
		
		if(_day > day)
			continue;
		else if(_day < day) {
			year = _year;
			month = _month;
			day = _day;
			continue;
		}
	}
	
	/* also closes fd */
	if(closedir(dirp) == -1)
		return -1;
	
	if(year == 9999 && month == 99 && day == 99) {
		APP_LOG_WARNING("\e[31mNothing to delete\e[0m\n");
		return -1;
	}
	
	char fname[16];
	sprintf(fname, "%04hu%02hu%02hu.tgz", year, month, day);
	int ret = unlink(fname);
	
	if(ret == -1) {
		APP_LOG_ERROR("\e[91mFailed to delete %s\e[0m\n", fname);
		return -1;
	}
	
	APP_LOG_INFO("\e[35mDeleted %s\e[0m\n", fname);
	
	return 0;
}
