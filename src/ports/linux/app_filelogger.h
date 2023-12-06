#ifndef APP_FILELOGGER_H
#define APP_FILELOGGER_H

/**
 * @file
 * @brief File logging interface
 *
 * Functions for creating and managing PLC variable logs
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "app_data.h"
#include "app_gsdml.h"
#include "osal.h"

#define ENTRY_SIZE (12 + APP_GSDML_VAR64_DATA_DIGITAL_SIZE)
#define ENTRY_BUFFER_SIZE (256*ENTRY_SIZE)

/* (circular) buffer for passing entries between threads */
typedef struct entry_buffer
{
	size_t start;
	size_t end;
	uint8_t buffer[ENTRY_BUFFER_SIZE];
} entry_buffer_t;

#define FILE_MIN_WRITE   4096
#define FILE_BUFFER_SIZE 32768

typedef struct log_file
{
	int fd;
	uint8_t buffer[FILE_BUFFER_SIZE];
	size_t buf_end;
	
	bool bigendian;
	uint8_t log_id[APP_GSDML_INSTALLATIONID_LENGTH];
	uint8_t type_list[APP_GSDML_DATATYPELIST_LENGTH];
	/* first, last timestamp ? */
} log_file_t;

#define LOG_THREAD_PRIORITY  12
#define LOG_THREAD_STACKSIZE 65536 /* bytes */

/**
 * Add a new entry to be logged.
 *
 * @param timestamp        In:    PLC timestamp of this entry
 * @param word_data        In:    Variable data array
 * @param word_count       In:    Number of words (2 bytes) in word_data
 * @return 0 on success, -1 on error
 */
int addLogEntry(
	DTL_data_t *timestamp,
	uint8_t *word_data,
	uint8_t word_count);

/**
 * Start a separate thread for logging I/O
 *
 * @param entries          In:    buffer that entries and mutex will exist in
 * @return 0 on success, -1 on error
 */
int initialiseLoggerThread(entry_buffer_t *entries);

/**
 * Main function for the logging thread
 *
 * @param arg              In     buffer that entries and mutex will exist in
 */
void log_thread_main(void * arg);

/**
 * Compare timestamps for whether they should belong to the same log
 *
 * @param ts_1             In     First timestamp
 * @param ts_2             In     Second timestamp
 */
bool DTLs_for_same_log(DTL_data_t *ts_1, DTL_data_t *ts_2);

/**
 * Start a new log in storage, assigning fd and flushing headers
 *
 * @param log_file         Out:   the new log
 * @return 0 on success, -1 on error
 */
int startLogFile(log_file_t *log_file, DTL_data_t *timeframe);
	
int writeLogHeader(log_file_t *log_file);

/**
 * Wrap up the current log and save/flush/sync it
 * @param log_file         In
 * @param flush            In: whether to sync before closing
 * @return 0 on sucess, -1 on error
 */
int finishLogFile(
	log_file_t *log_file,
	bool flush);

/**
 * Compress a day of logs, send it elsewhere, etc.
 * @param timeframe        In:     The day that was finished
 */
int finishLogGroup(DTL_data_t *timeframe);

void archive_thread_main(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* APP_FILELOGGER_H */
