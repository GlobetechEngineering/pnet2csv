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

static log_file_t current_log = {.fd = -1 };

int addLogEntry(
	DTL_data_t *timestamp,
	uint8_t *word_data,
	uint8_t word_count)
{
	if(current_log.fd < 0) {
		int ret = startLogFile(&current_log);
		if(ret == -1) {
			return -1;
		}
	}
	
	uint8_t entry[141];
	entry[0] = 0;
	
	uint16_t year;
	uint32_t nano;
	
	if(current_log.bigendian) {
		year = CC_TO_BE16(timestamp->year);
		nano = CC_TO_BE32(timestamp->nanosecond);
	}
	else {
		year = CC_TO_LE16(timestamp->year);
		nano = CC_TO_LE32(timestamp->nanosecond);
	}
	
	memcpy(entry+1, &year, 2);
	entry[3] = timestamp->month;
	entry[4] = timestamp->day;
	entry[5] = timestamp->weekday;
	entry[6] = timestamp->hour;
	entry[7] = timestamp->minute;
	entry[8] = timestamp->second;
	memcpy(entry+9, &nano, 4);
	
	memcpy(entry+13, word_data, 2*word_count);
	
	size_t written = write(current_log.fd, entry, sizeof(entry));
	if(written < sizeof(entry)) {
		/* there should be a buffer... */
		APP_LOG_WARNING(
			"Wrote %d/%d bytes\n",
			written, sizeof(entry)
		);
		return -1;
	}
	
	++current_log.lines;
	
	if(current_log.lines == INT_MAX) {
		int ret = finishLogFile(&current_log, true);
		if(ret == -1) {
			return -1;
		}
		/* finish is log-agnostic,
		so we need to say there is no current log now */
		current_log.fd = -1;
	}
	
	return 0;
}

int startLogFile(
	log_file_t *log_file)
{
	log_file_t new_log;
	app_read_log_parameters(new_log.log_id, new_log.type_list);
	
	APP_LOG_INFO("Starting new log\n");
	
	int fd = open(
		"log0000.bin",
		O_WRONLY | O_APPEND | O_CREAT | O_TRUNC,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH /* owner RW, others R */
	);
	
	if(fd < 0) {
		/* didn't work :( */
		/* try harder, or just */
		return -1;
	}
	
	new_log.fd = fd;
	new_log.bigendian = true;
	new_log.lines = 0;
	int ret = writeLogHeader(&new_log);
	if(ret == -1) {
		return -1;
	}
	
	/*
	this is probably by value
	make current_log a *log_file_t ?
	*/
	current_log = new_log;
	return 0;
}

int writeLogHeader(log_file_t *log_file) {
	/* todo... less of a magic number,
	or add buffering for everything
	and write directly using those functions
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
		/* clearly this is a disaster */
		return -1;
	}
	
	/* This blocks :O */
	if(fsync(log_file->fd) == -1) {
		if(errno == EBADF || errno == ENOSPC) {
			return -1;
		}
	}
	
	return 0;
}

int finishLogFile(log_file_t *log_file, bool flush)
{
	uint8_t end[1] = {255};
	size_t written = write(log_file->fd, end, sizeof(end));
	if(written < sizeof(end)) {
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
