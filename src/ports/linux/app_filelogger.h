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

#include <dirent.h>

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
} log_file_t;

#define LOG_THREAD_PRIORITY  12
#define LOG_THREAD_STACKSIZE 65536 /* bytes */

#define ARCHIVE_PRIORITY 8

/* delete old logs when too few blocks are available */
#define FREE_SPACE_PERCENT 20

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
 * Compare timestamps for whether they should belong to the same log
 *
 * @param ts_1             In     First timestamp
 * @param ts_2             In     Second timestamp
 */
bool DTLs_for_same_log(DTL_data_t *ts_1, DTL_data_t *ts_2);

/**
 * Provide file descriptor for the log folder, creating the relevant directories
 * if necessary, and reusing the descriptor if already open
 *
 * @return The file descriptor, -1 on error
 */
int getLogDir();

/**
 * Open a new directory stream for the log folder,
 * for use with readdir
 *
 * @return The directory pointer, NULL on error
 */
DIR *openLogDir();

/**
 * Start a new log in storage, assigning fd and flushing headers
 *
 * @param log_file         Out:   the new log
 * @return 0 on success, -1 on error
 */
int startLogFile(log_file_t *log_file, DTL_data_t *timeframe);

/**
 * Write non-repeated data into log file, attempting sync
 *
 * @param log_file         In
 * @return 0 on success, -1 on error
 */
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
 * Manage space by clearing old logs and compressing the new day
 * @param timeframe        In:     The day that was finished
 */
int finishLogGroup(DTL_data_t *timeframe);

/**
 * Create a compressed archive of the directory and delete the original
 * @param directory        In: Name of the directory under log directory
 *
 * @return 0 on success, -1 on error
 */
int compressDirectory(char *directory);

/**
 * Identifies the oldest archive and deletes it
 *
 * @return 0 on sucess, -1 on error
 */
int deleteOldest();

#ifdef __cplusplus
}
#endif

#endif /* APP_FILELOGGER_H */
