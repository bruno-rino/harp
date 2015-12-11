/*
 * Copyright (C) 2015 S[&]T, The Netherlands.
 *
 * This file is part of HARP.
 *
 * HARP is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * HARP is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HARP; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "harp-internal.h"

#include "coda-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <string.h>

#define MAX_ERROR_INFO_LENGTH	4096

static char harp_error_message_buffer[MAX_ERROR_INFO_LENGTH + 1];

/** \defgroup harp_error HARP Error
 * With a few exceptions almost all HARP functions return an integer that indicate whether the function was able to
 * perform its operations successfully. The return value will be 0 on success and -1 otherwise. In case you get a -1
 * you can look at the global variable #harp_errno for a precise error code. Each error code and its meaning is
 * described in this section. You will also be able to retrieve a character string with an error description via
 * the harp_errno_to_string() function. This function will return either the default error message for the error
 * code, or a custom error message. A custom error message will only be returned if the error code you pass to
 * harp_errno_to_string() is equal to the last error that occurred and if this last error was set with a custom error
 * message. The HARP error state can be set with the harp_set_error() function.<br>
 */

/** \addtogroup harp_error
 * @{
 */

/** \name Error values
 * \ingroup harp_error
 * @{
 */

/** \def HARP_SUCCESS
 * Success (no error).
 */
/** \def HARP_ERROR_OUT_OF_MEMORY
 * Out of memory.
 */
/** \def HARP_ERROR_HDF4
 * An error occurred in the HDF4 library.
 */
/** \def HARP_ERROR_HDF5
 * An error occurred in the HDF5 library.
 */
/** \def HARP_ERROR_NETCDF
 * An error occurred in the netCDF library.
 */
/** \def HARP_ERROR_CODA
 * An error occurred in the CODA library.
 */

/** \def HARP_ERROR_FILE_NOT_FOUND
 * File not found.
 */
/** \def HARP_ERROR_FILE_OPEN
 * Could not open file.
 */
/** \def HARP_ERROR_FILE_CLOSE
 * Could not close file.
 */
/** \def HARP_ERROR_FILE_READ
 * Could not read data from file.
 */
/** \def HARP_ERROR_FILE_WRITE
 * Could not write data to file.
 */

/** \def HARP_ERROR_INVALID_ARGUMENT
 * Invalid argument.
 */
/** \def HARP_ERROR_INVALID_INDEX
 * Invalid index argument.
 */
/** \def HARP_ERROR_INVALID_NAME
 * Invalid name argument.
 */
/** \def HARP_ERROR_INVALID_FORMAT
 * Invalid format in argument.
 */
/** \def HARP_ERROR_INVALID_DATETIME
 * Invalid date/time argument.
 */
/** \def HARP_ERROR_INVALID_TYPE
 * Invalid type.
 */
/** \def HARP_ERROR_ARRAY_NUM_DIMS_MISMATCH
 * Incorrect number of dimensions argument.
 */
/** \def HARP_ERROR_ARRAY_OUT_OF_BOUNDS
 * Array index out of bounds.
 */
/** \def HARP_ERROR_VARIABLE_NOT_FOUND
 * Variable not found.
 */

/** \def HARP_ERROR_UNIT_CONVERSION
 * An error occured in the unit conversion.
 */

/** \def HARP_ERROR_PRODUCT
 * There was an error detected in the product.
 */

/** \def HARP_ERROR_SCRIPT
 * There was an error detected in the script.
 */
/** \def HARP_ERROR_SCRIPT_SYNTAX
 * There is a syntax error in the script.
 */

/** \def HARP_ERROR_INGESTION
 * There was an error in the ingestion of a data product.
 */
/** \def HARP_ERROR_INGESTION_OPTION_SYNTAX
 * There was a syntax error in the ingestion option.
 */
/** \def HARP_ERROR_INVALID_INGESTION_OPTION
 * The ingestion option is not valid for this ingestion.
 */
/** \def HARP_ERROR_INVALID_INGESTION_OPTION_VALUE
 * The ingestion option has value that is not valid for this option.
 */

/** \def HARP_ERROR_NO_DATA
 * The operation resulted in an 'empty' product.
 */

/** @} */

/** Variable that contains the error type.
 * If no error has occurred the variable contains #HARP_SUCCESS (0).
 * \hideinitializer
 */
LIBHARP_API int harp_errno = HARP_SUCCESS;

/** @} */

static void add_error_message_vargs(const char *message, va_list ap)
{
    size_t current_length;

    if (message == NULL)
    {
        return;
    }

    current_length = strlen(harp_error_message_buffer);
    if (current_length >= MAX_ERROR_INFO_LENGTH)
    {
        return;
    }
    vsnprintf(&harp_error_message_buffer[current_length], MAX_ERROR_INFO_LENGTH - current_length, message, ap);
    harp_error_message_buffer[MAX_ERROR_INFO_LENGTH] = '\0';
}

static int add_error_message(const char *message, ...)
{
    va_list ap;

    va_start(ap, message);
    add_error_message_vargs(message, ap);
    va_end(ap);

    return 0;
}

static void set_error_message_vargs(const char *message, va_list ap)
{
    if (message == NULL)
    {
        harp_error_message_buffer[0] = '\0';
    }
    else
    {
        vsnprintf(harp_error_message_buffer, MAX_ERROR_INFO_LENGTH, message, ap);
        harp_error_message_buffer[MAX_ERROR_INFO_LENGTH] = '\0';
    }
}

void harp_add_coda_cursor_path_to_error_message(const coda_cursor *cursor)
{
    harp_add_error_message(" at '/");
    coda_cursor_print_path(cursor, add_error_message);
    harp_add_error_message("'");
}

/** \addtogroup harp_error
 * @{
 */

/** Extend the current error message with additional information.
 * \param message Error message using printf() format.
 */
LIBHARP_API void harp_add_error_message(const char *message, ...)
{
    va_list ap;

    va_start(ap, message);
    add_error_message_vargs(message, ap);
    va_end(ap);
}

/** Set the error value and optionally set a custom error message.
 * If \a message is NULL then the default error message for the error number will be used.
 * \param err Value of #harp_errno.
 * \param message Optional error message using printf() format.
 */
LIBHARP_API void harp_set_error(int err, const char *message, ...)
{
    va_list ap;

    harp_errno = err;

    va_start(ap, message);
    set_error_message_vargs(message, ap);
    va_end(ap);

    if (err == HARP_ERROR_HDF4 && message == NULL)
    {
        harp_hdf4_add_error_message();
    }
    if (err == HARP_ERROR_HDF5 && message == NULL)
    {
        harp_hdf5_add_error_message();
    }
    if (err == HARP_ERROR_CODA && message == NULL)
    {
        harp_add_error_message("%s", coda_errno_to_string(coda_errno));
    }
}

/** Returns a string with the description of the HARP error.
 * If \a err equals the current HARP error status then this function will return the error message that was last set
 * using harp_set_error(). If the error message argument to harp_set_error() was NULL or if \a err does not equal the
 * current HARP error status then the default error message for \a err will be returned.
 * \param err Value of #harp_errno.
 * \return String with a description of the HARP error.
 */
LIBHARP_API const char *harp_errno_to_string(int err)
{
    if (err == harp_errno && harp_error_message_buffer[0] != '\0')
    {
        /* return the custom error message for the current HARP error */
        return harp_error_message_buffer;
    }
    else
    {
        switch (err)
        {
            case HARP_SUCCESS:
                return "success (no error)";
            case HARP_ERROR_OUT_OF_MEMORY:
                return "out of memory";

            case HARP_ERROR_HDF4:
                return "HDF4 error";
            case HARP_ERROR_HDF5:
                return "HDF5 error";
            case HARP_ERROR_NETCDF:
                return "netCDF error";
            case HARP_ERROR_CODA:
                return "CODA error";

            case HARP_ERROR_FILE_NOT_FOUND:
                return "file not found";
            case HARP_ERROR_FILE_OPEN:
                return "error opening file";
            case HARP_ERROR_FILE_CLOSE:
                return "error closing file";
            case HARP_ERROR_FILE_READ:
                return "error reading file";
            case HARP_ERROR_FILE_WRITE:
                return "error writing file";

            case HARP_ERROR_INVALID_ARGUMENT:
                return "invalid argument";
            case HARP_ERROR_INVALID_INDEX:
                return "invalid index";
            case HARP_ERROR_INVALID_NAME:
                return "invalid name";
            case HARP_ERROR_INVALID_FORMAT:
                return "invalid format";
            case HARP_ERROR_INVALID_DATETIME:
                return "invalid date/time";
            case HARP_ERROR_INVALID_TYPE:
                return "invalid type";
            case HARP_ERROR_ARRAY_NUM_DIMS_MISMATCH:
                return "incorrect number of dimensions";
            case HARP_ERROR_ARRAY_OUT_OF_BOUNDS:
                return "array index out of bounds";
            case HARP_ERROR_VARIABLE_NOT_FOUND:
                return "variable not found";

            case HARP_ERROR_UNIT_CONVERSION:
                return "unit conversion error";

            case HARP_ERROR_PRODUCT:
                return "product error";

            case HARP_ERROR_SCRIPT:
                return "script error";
            case HARP_ERROR_SCRIPT_SYNTAX:
                return "syntax error in script";

            case HARP_ERROR_INGESTION:
                return "ingestion error";
            case HARP_ERROR_INGESTION_OPTION_SYNTAX:
                return "syntax error in ingestion option";
            case HARP_ERROR_INVALID_INGESTION_OPTION:
                return "invalid ingestion option";
            case HARP_ERROR_INVALID_INGESTION_OPTION_VALUE:
                return "invalid ingestion option value";

            case HARP_ERROR_NO_DATA:
                return "no data left after operation";

            default:
                if (err == harp_errno)
                {
                    return harp_error_message_buffer;
                }
                else
                {
                    return "";
                }
        }
    }
}

/** @} */