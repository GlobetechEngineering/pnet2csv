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

typedef struct log_file
{
	int fd;
	
	bool bigendian;
	uint8_t log_id[APP_GSDML_INSTALLATIONID_LENGTH];
	uint8_t type_list[APP_GSDML_DATATYPELIST_LENGTH];
	/* first, last timestamp ? */
	
	int lines;
} log_file_t;

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
 * Start a new log, writing the header
 *
 * @param log_file         Out:   the new log
 * @return 0 on success, -1 on error
 */
int startLogFile(
	log_file_t *log_file);
	
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

#ifdef __cplusplus
}
#endif

#endif /* APP_FILELOGGER_H */
