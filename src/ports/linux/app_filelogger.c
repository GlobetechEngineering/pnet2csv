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
#include <errno.h>

#include <time.h>

static os_mutex_t *mutex;
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
	
	/*
	OSAL does not wrap pthread_mutex_timedlock
	This is running on the important thread so we'd like to fail fairly quickly
	Could write a wrapper, or just call directly w/ cast as that's all OSAL does
	or just ensure the logging thread doesn't hold it for too long...
	*/
	os_mutex_lock(mutex);
	
	if((entries.end + ENTRY_SIZE)%ENTRY_BUFFER_SIZE == entries.start) {
		/*
		"no more" room
		strictly speaking one more will fit,
		but will make the buffer look empty (start = end)
		*/
		os_mutex_unlock(mutex);
		APP_LOG_WARNING("Log buffer full - dropping entries!\n");
		return -1;
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
				
				if(current_log.fd >= 0)
					finishLogFile(&current_log, true);
				
				curr_log_start = entry_ts;
				startLogFile(&current_log, &curr_log_start);
				
				/* ready to process again */
				os_mutex_lock(mutex);
			}
			
			if(current_log.buf_end + (ENTRY_SIZE + 1) > FILE_BUFFER_SIZE) {
				/* oh no! */
				APP_LOG_WARNING("File buffer running low (%d/%d)\n", current_log.buf_end, FILE_BUFFER_SIZE);
				break;
			}
			current_log.buffer[current_log.buf_end] = 0;
			memcpy(current_log.buffer +current_log.buf_end+1, entry, ENTRY_SIZE);
			current_log.buf_end += (ENTRY_SIZE + 1);

			entries->start = (entries->start + ENTRY_SIZE) % ENTRY_BUFFER_SIZE;
		}
		
		os_mutex_unlock(mutex);
		
		/* write log if buffer is long enough */
		
		while(current_log.buf_end >= FILE_WRITE_SIZE) {
			size_t written = write(current_log.fd, current_log.buffer, FILE_WRITE_SIZE);
			
			memmove(current_log.buffer, current_log.buffer+written, current_log.buf_end - written);
			
			current_log.buf_end -= written;
		}
		
		/* wait on something */
		
		os_usleep(5000);
	}
}

bool DTLs_for_same_log(DTL_data_t *ts_1, DTL_data_t *ts_2)
{
	if(    ts_1->year == ts_2->year
		&& ts_1->month == ts_2->month
		&& ts_1->day == ts_2->day
		&& ts_1->hour == ts_2->hour
		&& ts_1->minute/10 == ts_2->minute/10
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
	
	char fname[32];
	sprintf(fname, "%4d%02d%02d_%02d%02d.bin",
		timeframe->year, timeframe->month, timeframe->day,
		timeframe->hour, 10*(timeframe->minute/10)
	);
	
	int fd = open(
		fname,
		O_WRONLY | O_APPEND | O_CREAT | O_EXCL,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH /* owner RW, others R */
	);
	
	for(int i=2; fd < 0 && errno == EEXIST && i<=9; i++) {
		fname[13] = '_';
		fname[14] = i + '0';
		fname[15] = '.';
		fname[16] = 'b';
		fname[17] = 'i';
		fname[18] = 'n';
		fname[19] = 0;
		
		fd = open(
			fname,
			O_WRONLY | O_APPEND | O_CREAT | O_EXCL,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH /* owner RW, others R */
		);
	}
	
	if(fd < 0) {
		/* didn't work :( */
		/* try harder, or just */
		return -1;
	}
	
	APP_LOG_INFO("Starting %s\n", fname);
	
	log_file->fd = fd;
	log_file->bigendian = true;
	
	int ret = writeLogHeader(log_file);
	if(ret == -1) {
		return -1;
	}
	
	return 0;
}

int writeLogHeader(log_file_t *log_file) {
	/* todo...probably write this straight into the buffer
	*/
	uint8_t header[81];
	
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
	header[16] = APP_GSDML_VAR64_DATA_DIGITAL_SIZE/2;
	
	/* data types */
	memcpy(header+17, log_file->type_list, APP_GSDML_DATATYPELIST_LENGTH);
	
	/* all done, write it */
	size_t written = write(log_file->fd, header, sizeof(header));
	
	if(written < sizeof(header)) {
		APP_LOG_WARNING(
			"Header: wrote %d/%d bytes\n",
			written, sizeof(header)
		);
		
		/* keep the rest for later */
		size_t remnant = sizeof(header) - written;
		/* there shouldn't be anything in the buffer, but is it wise to assert that? */
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
	log_file->buffer[log_file->buf_end] = 255;
	log_file->buf_end += 1;
	
	size_t written = write(log_file->fd, log_file->buffer, log_file->buf_end);
	if(written < log_file->buf_end) {
		/* incredible */
		return -1;
	}
	
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
	
	return 0;
}
