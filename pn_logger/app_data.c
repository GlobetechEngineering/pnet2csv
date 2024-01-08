/*********************************************************************
 *        _       _         _
 *  _ __ | |_  _ | |  __ _ | |__   ___
 * | '__|| __|(_)| | / _` || '_ \ / __|
 * | |   | |_  _ | || (_| || |_) |\__ \
 * |_|    \__|(_)|_| \__,_||_.__/ |___/
 *
 * www.rt-labs.com
 * Copyright 2021 rt-labs AB, Sweden.
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 ********************************************************************/

#include "app_data.h"
#include "app_utils.h"
#include "app_gsdml.h"
#include "app_log.h"
#include "logger_common.h"
#include "osal.h"
#include "pnal.h"
#include <pnet_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_DATA_DEFAULT_OUTPUT_DATA 0

/* Parameter data for digital submodules
 * The stored value is shared between all digital submodules in this example.
 *
 * Todo: Data is always in pnio data format. Add conversion to uint32_t.
 */
 
static uint8_t installationid[APP_GSDML_INSTALLATIONID_LENGTH] = {0};

/* Digital submodule process data
 * The GSD dictates there's only one module, so it better be alright to use single variables */
static uint8_t variabledata[APP_GSDML_VAR64_DATA_DIGITAL_SIZE] = {0};
static DTL_data_t PLCtimestamp = {0};

uint8_t * app_data_get_input_data (
   uint16_t slot_nbr,
   uint16_t subslot_nbr,
   uint32_t submodule_id,
   bool button_pressed,
   uint16_t * size,
   uint8_t * iops)
{
	/*
	logger does not send input data 
	Otherwise, this would determine what submodule's input is requested and return the appropriate data
	*/
	
   /* Automated RT Tester scenario 2 - unsupported (sub)module */
   return NULL;
}

int app_data_set_output_data (
   uint16_t slot_nbr,
   uint16_t subslot_nbr,
   uint32_t submodule_id,
   uint8_t * data,
   uint16_t size)
{
    if (data == NULL)
    {
	    return -1;
    }
   
    if (submodule_id == APP_GSDML_SUBMOD_ID_LOGTS) {
		if(size == APP_GSDML_TIMESTAMP_SIZE) {
			memcpy(&PLCtimestamp.year,       data,   2);
			memcpy(&PLCtimestamp.month,      data+2, 1);
			memcpy(&PLCtimestamp.day,        data+3, 1);
			memcpy(&PLCtimestamp.weekday,    data+4, 1);
			memcpy(&PLCtimestamp.hour,       data+5, 1);
			memcpy(&PLCtimestamp.minute,     data+6, 1);
			memcpy(&PLCtimestamp.second,     data+7, 1);
			memcpy(&PLCtimestamp.nanosecond, data+8, 4);
			
			// convert from network endian
			PLCtimestamp.year = CC_FROM_BE16(PLCtimestamp.year);
			PLCtimestamp.nanosecond = CC_FROM_BE32(PLCtimestamp.nanosecond);
			
			return 0;
		}
    }
	else if (submodule_id == APP_GSDML_SUBMOD_ID_LOGW64) {
		if(size == APP_GSDML_VAR64_DATA_DIGITAL_SIZE) {
			memcpy(variabledata, data, size);
			
			return 0;
		}
	}

   return -1;
}

int app_data_set_default_outputs (void)
{
   variabledata[0] = APP_DATA_DEFAULT_OUTPUT_DATA;
   return 0;
}

int app_data_write_parameter (
   uint16_t slot_nbr,
   uint16_t subslot_nbr,
   uint32_t submodule_id,
   uint32_t index,
   const uint8_t * data,
   uint16_t length)
{
   const app_gsdml_param_t * par_cfg;

   par_cfg = app_gsdml_get_parameter_cfg (submodule_id, index);
   if (par_cfg == NULL)
   {
      APP_LOG_WARNING (
         "PLC write request unsupported submodule/parameter. "
         "Submodule id: %u Index: %u\n",
         (unsigned)submodule_id,
         (unsigned)index);
      return -1;
   }

   if (length != par_cfg->length)
   {
      APP_LOG_WARNING (
         "PLC write request unsupported length. "
         "Index: %u Length: %u Expected length: %u\n",
         (unsigned)index,
         (unsigned)length,
         par_cfg->length);
      return -1;
   }
   
    if(index == APP_GSDML_PARAMETER_INSTALLATIONID_IDX) {
	    memcpy(&installationid, data, length);
    }
	
   APP_LOG_DEBUG ("  Writing parameter \"%s\"\n", par_cfg->name);
   app_log_print_bytes (APP_LOG_LEVEL_DEBUG, data, length);

   return 0;
}

int app_data_read_parameter (
   uint16_t slot_nbr,
   uint16_t subslot_nbr,
   uint32_t submodule_id,
   uint32_t index,
   uint8_t ** data,
   uint16_t * length)
{
   const app_gsdml_param_t * par_cfg;

   par_cfg = app_gsdml_get_parameter_cfg (submodule_id, index);
   if (par_cfg == NULL)
   {
      APP_LOG_WARNING (
         "PLC read request unsupported submodule/parameter. "
         "Submodule id: %u Index: %u\n",
         (unsigned)submodule_id,
         (unsigned)index);
      return -1;
   }

   if (*length < par_cfg->length)
   {
      APP_LOG_WARNING (
         "PLC read request unsupported length. "
         "Index: %u Max length: %u Data length for our parameter: %u\n",
         (unsigned)index,
         (unsigned)*length,
         par_cfg->length);
      return -1;
   }

   APP_LOG_DEBUG ("  Reading \"%s\"\n", par_cfg->name);
   
    if(index == APP_GSDML_PARAMETER_INSTALLATIONID_IDX) {
		*data = (uint8_t *) &installationid;
		*length = sizeof (installationid);
	}
   
   app_log_print_bytes (APP_LOG_LEVEL_DEBUG, *data, *length);

   return 0;
}

int app_read_log_parameters (
	uint8_t parm_installid[APP_GSDML_INSTALLATIONID_LENGTH])
{
	/* Make a copy */
	memcpy(parm_installid, installationid, APP_GSDML_INSTALLATIONID_LENGTH);
	
	return 0;
}

int app_read_log_data(
	DTL_data_t *data_timestamp,
	uint8_t data_variables[APP_GSDML_VAR64_DATA_DIGITAL_SIZE])
{
	*data_timestamp = PLCtimestamp;
	memcpy(data_variables, variabledata, APP_GSDML_VAR64_DATA_DIGITAL_SIZE);
	
	return 0;
}
