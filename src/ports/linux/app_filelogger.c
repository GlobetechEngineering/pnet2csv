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
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

static os_mutex_t *mutex;
static os_sem_t *finishDataSemaphore;
static entry_buffer_t entries;
static bool bigendian = true;

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
	static unsigned int next_logged_drop = 1;
	
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
			next_logged_drop *= 2;
		}
		return -1;
	}
	
	if(drop_count != 0) {
		APP_LOG_WARNING("[%2d:%2d] Recovered after \e[31m%u\e[0m dropped.\n", timestamp->hour, timestamp->minute, drop_count);
	
		drop_count = 0;
		next_logged_drop = 1;
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
	
	APP_LOG_DEBUG("Hello from the logging thread\n");
	
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
					APP_LOG_INFO("\nretrying... ");
					os_usleep(500);
					ret = startLogFile(&current_log, &curr_log_start);
				}
				APP_LOG_INFO("fd=%d\n", current_log.fd);
				
				/* ready to process again */
				os_mutex_lock(mutex);
			}
			
			if(current_log.fd == -1) {
				APP_LOG_ERROR("No file?\n");
				break;
			}
			
			if(current_log.buf_end + (ENTRY_SIZE + 1) >= FILE_BUFFER_SIZE) {
				/* Not losing anything yet, but the buffer needs something cleared before copying any more */
				APP_LOG_WARNING("File buffer running low (%d/%d)\n", current_log.buf_end, FILE_BUFFER_SIZE);
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

int startLogFile(log_file_t *log_file, DTL_data_t *timeframe)
{
	/* reusing log_file because the buffers are large */
	log_file->buf_end = 0;
	app_read_log_parameters(log_file->log_id, log_file->type_list);
	
	char date[16];
	sprintf(date, "%4d%02d%02d", timeframe->year, timeframe->month, timeframe->day);
	
	int ret = mkdir(date, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
	if(ret == -1 && errno != EEXIST) {
		return -1;
	}
	
	/* O_PATH undefined ? */
	int dirfd = open(date, O_DIRECTORY | O_CLOEXEC);
	if(dirfd == -1) {
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
	
	ret = close(dirfd);
	
	if(fd < 0) {
		APP_LOG_WARNING("Opened %s but failed to close\n", date);
		return -1;
	}
	
	APP_LOG_INFO("Starting %s/%s... ", date, fname);
	
	log_file->fd = fd;
	log_file->bigendian = true;
	
	ret = writeLogHeader(log_file);
	if(ret == -1) {
		return -1;
	}
	
	return 0;
}

int writeLogHeader(log_file_t *log_file)
{
	unsigned int header_size = 4+3+1+APP_GSDML_INSTALLATIONID_LENGTH+1+APP_GSDML_DATATYPELIST_LENGTH;
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
	/* digital size is bytes and words are 2
	   could use datatypelist length instead ...
	*/
	header[8+APP_GSDML_INSTALLATIONID_LENGTH] = APP_GSDML_VAR64_DATA_DIGITAL_SIZE/2;
	
	/* data types */
	memcpy(header+9+APP_GSDML_INSTALLATIONID_LENGTH, log_file->type_list, APP_GSDML_DATATYPELIST_LENGTH);
	
	/* all done, write it */
	ssize_t written = write(log_file->fd, header, header_size);
	if(written == -1) {
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
	
	if(fsync(log_file->fd) == -1) {
		if(errno == EBADF || errno == ENOSPC) {
			return -1;
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
			continue;
		}
		start += written;
	}
	
	/* close does not flush, so this does make a difference */
	if(flush) {
		int ret = fsync(log_file->fd);
		if(ret == -1) {
			return -1;
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
	
	char directory[16], archive[24];
	sprintf(directory, "%4d%02d%02d", timeframe->year, timeframe->month, timeframe->day);
	sprintf(archive, "%s.tgz", directory);
	
	/* got the data, let the logging thread continue */
	os_sem_signal(finishDataSemaphore);
	
	#if defined(USE_SCHED_FIFO)
	/* change scheduling policy to normal */
	struct sched_param schedparam = {0};
	int ret = pthread_setschedparam(pthread_self(), SCHED_OTHER, &schedparam);
	if(ret != 0) {
		APP_LOG_WARNING("\e[33mCould not change scheduling policy of archiving thread\e[0m - running real time at lower priority\n");
	}
	#endif
	
	pid_t child_pid;
	int status;
	
	child_pid = fork();
	if(child_pid == 0) {
		/* this is the child */
		
		#if !defined(USE_SCHED_FIFO)
		/* be very adamant that this is a low priority
		   SCHED_IDLE could be even better, occuring below everything
		   Note that RT policies do not use nice values */
		nice(20);
		#endif
		
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
		APP_LOG_ERROR("\e[31mFailed to instantiate archiving of %s\e[0m\n", directory);
		return;
	}
	
	APP_LOG_INFO("Archiving %s...\n", directory);
	
	waitpid(child_pid, &status, 0);
	
	if(WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		APP_LOG_ERROR("\e[31mFailed to archive %s\e[0m\n", archive);
		/* os_thread_create doesn't want us to return a value */
		return;
	}
	
	/* deleting a folder of files is a nuisance... just use rm */
	child_pid = fork();
	if(child_pid == 0) {
		/* this is the child */
		
		char *argv[] = { "rm", "-rf", directory, NULL };
		
		/* p searches so we don't have to */
		execvp("rm", argv);
		
		/* exec didn't replace us... don't want this process hanging around */
		/* Goodbye, world! */
		exit(EXIT_FAILURE);
	}
	else if(child_pid == -1) {
		APP_LOG_ERROR("\e[31mFailed to delete %s\e[0m\n", directory);
		return;
	}
	
	waitpid(child_pid, &status, 0);
	
	if(WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		APP_LOG_ERROR("\e[31mFailed to delete %s\e[0m\n", directory);
		return;
	}
	
	APP_LOG_INFO("\e[32mArchived %s as \e[92m%s\e[0m\n", directory, archive);
}
