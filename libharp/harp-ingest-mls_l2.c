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

#include "coda.h"
#include "harp-ingestion.h"

#include <stdlib.h>
#include <string.h>

#define SECONDS_FROM_1993_TO_2000 (220838400 + 5)

typedef struct ingest_info_struct
{
    const char *swath_name;
    coda_product *product;
    coda_cursor swath_cursor;
    coda_cursor geo_cursor;
    long num_times;
    long num_levels;
} ingest_info;

static int init_cursors(ingest_info *info)
{
    if (coda_cursor_set_product(&info->swath_cursor, info->product) != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    if (coda_cursor_goto(&info->swath_cursor, "/HDFEOS/SWATHS") != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    if (coda_cursor_goto_record_field_by_name(&info->swath_cursor, info->swath_name) != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    info->geo_cursor = info->swath_cursor;
    if (coda_cursor_goto_record_field_by_name(&info->swath_cursor, "Data_Fields") != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    if (coda_cursor_goto_record_field_by_name(&info->geo_cursor, "Geolocation_Fields") != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }

    return 0;
}

static int get_dimensions(ingest_info *info)
{
    coda_cursor cursor;
    long coda_dimension[CODA_MAX_NUM_DIMS];
    int num_coda_dimensions;

    cursor = info->swath_cursor;
    if (coda_cursor_goto_record_field_by_name(&cursor, "L2gpValue") != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    if (coda_cursor_get_array_dim(&cursor, &num_coda_dimensions, coda_dimension) != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }

    info->num_times = coda_dimension[0];
    info->num_levels = coda_dimension[1];

    return 0;
}

static int get_variable_attributes(coda_cursor *cursor, double *missing_value, double *scale_factor, double *offset)
{
    if (coda_cursor_goto_attributes(cursor) != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    if (coda_cursor_goto_record_field_by_name(cursor, "MissingValue") != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    if (coda_cursor_goto_first_array_element(cursor) != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    if (coda_cursor_read_double(cursor, missing_value) != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    coda_cursor_goto_parent(cursor);
    coda_cursor_goto_parent(cursor);

    if (coda_cursor_goto_record_field_by_name(cursor, "ScaleFactor") != 0)
    {
        /* use a scale factor of 1 */
        *scale_factor = 1;
    }
    else
    {
        if (coda_cursor_goto_first_array_element(cursor) != 0)
        {
            harp_set_error(HARP_ERROR_CODA, NULL);
            return -1;
        }
        if (coda_cursor_read_double(cursor, scale_factor) != 0)
        {
            harp_set_error(HARP_ERROR_CODA, NULL);
            return -1;
        }
        coda_cursor_goto_parent(cursor);
        coda_cursor_goto_parent(cursor);
    }

    if (coda_cursor_goto_record_field_by_name(cursor, "Offset") != 0)
    {
        /* use an offset of 0 */
        *offset = 0;
    }
    else
    {
        if (coda_cursor_goto_first_array_element(cursor) != 0)
        {
            harp_set_error(HARP_ERROR_CODA, NULL);
            return -1;
        }
        if (coda_cursor_read_double(cursor, offset) != 0)
        {
            harp_set_error(HARP_ERROR_CODA, NULL);
            return -1;
        }
        coda_cursor_goto_parent(cursor);
        coda_cursor_goto_parent(cursor);
    }
    coda_cursor_goto_parent(cursor);

    return 0;
}

static int read_variable(coda_cursor *cursor, const char *name, int num_dimensions, long dimension_0, long dimension_1,
                         harp_array data)
{
    double missing_value;
    double scale_factor;
    double offset;
    long num_elements;
    long coda_dimension[CODA_MAX_NUM_DIMS];
    int num_coda_dimensions;
    long i;

    if (coda_cursor_goto_record_field_by_name(cursor, name) != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    if (coda_cursor_get_array_dim(cursor, &num_coda_dimensions, coda_dimension) != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }
    if (num_coda_dimensions != num_dimensions)
    {
        harp_set_error(HARP_ERROR_PRODUCT, "product error detected in MLS L2 product (variable %s has %d dimensions, "
                       "expected %d)", name, num_coda_dimensions, num_dimensions);
        return -1;
    }
    if (dimension_0 != coda_dimension[0])
    {
        harp_set_error(HARP_ERROR_PRODUCT, "product error detected in MLS L2 product (first dimension for variable %s "
                       "has %ld elements, expected %ld", name, coda_dimension[0], dimension_0);
        return -1;
    }
    num_elements = coda_dimension[0];
    if (num_dimensions > 1)
    {
        if (dimension_1 != coda_dimension[1])
        {
            harp_set_error(HARP_ERROR_PRODUCT, "product error detected in MLS L2 product (second dimension for "
                           "variable %s has %ld elements, expected %ld", name, coda_dimension[1], dimension_1);
            return -1;
        }
        num_elements *= coda_dimension[1];
    }
    if (get_variable_attributes(cursor, &missing_value, &scale_factor, &offset) != 0)
    {
        return -1;
    }
    if (coda_cursor_read_double_array(cursor, data.double_data, coda_array_ordering_c) != 0)
    {
        harp_set_error(HARP_ERROR_CODA, NULL);
        return -1;
    }

    /* apply scaling and filter for NaN */
    for (i = 0; i < num_elements; i++)
    {
        if (data.double_data[i] == missing_value)
        {
            data.double_data[i] = coda_NaN();
        }
        else
        {
            data.double_data[i] = offset + scale_factor * data.double_data[i];
        }
    }

    coda_cursor_goto_parent(cursor);

    return 0;
}

static int read_dimensions(void *user_data, long dimension[HARP_NUM_DIM_TYPES])
{
    ingest_info *info = (ingest_info *)user_data;

    dimension[harp_dimension_time] = info->num_times;
    dimension[harp_dimension_vertical] = info->num_levels;

    return 0;
}

static int read_time(void *user_data, harp_array data)
{
    ingest_info *info = (ingest_info *)user_data;
    long i;

    if (read_variable(&info->geo_cursor, "Time", 1, info->num_times, 0, data) != 0)
    {
        return -1;
    }

    /* convert time values from TAI93 to seconds since 2000-01-01 */
    for (i = 0; i < info->num_times; i++)
    {
        data.double_data[i] -= SECONDS_FROM_1993_TO_2000;
    }

    return 0;
}

static int read_longitude(void *user_data, harp_array data)
{
    ingest_info *info = (ingest_info *)user_data;

    return read_variable(&info->geo_cursor, "Longitude", 1, info->num_times, 0, data);
}

static int read_latitude(void *user_data, harp_array data)
{
    ingest_info *info = (ingest_info *)user_data;

    return read_variable(&info->geo_cursor, "Latitude", 1, info->num_times, 0, data);
}

static int read_pressure(void *user_data, harp_array data)
{
    ingest_info *info = (ingest_info *)user_data;

    return read_variable(&info->geo_cursor, "Pressure", 1, info->num_levels, 0, data);
}

static int read_value(void *user_data, harp_array data)
{
    ingest_info *info = (ingest_info *)user_data;

    return read_variable(&info->swath_cursor, "L2gpValue", 2, info->num_times, info->num_levels, data);
}

static int read_error(void *user_data, harp_array data)
{
    ingest_info *info = (ingest_info *)user_data;

    return read_variable(&info->swath_cursor, "L2gpPrecision", 2, info->num_times, info->num_levels, data);
}

static void ingestion_done(void *user_data)
{
    ingest_info *info = (ingest_info *)user_data;

    if (info != NULL)
    {
        free(info);
    }
}

static int ingestion_init(const harp_ingestion_module *module, coda_product *product,
                          const harp_ingestion_options *options, harp_product_definition **definition, void **user_data,
                          const char *swath_name)
{
    ingest_info *info;

    (void)options;

    info = malloc(sizeof(ingest_info));
    if (info == NULL)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not allocate %lu bytes) (%s:%u)",
                       sizeof(ingest_info), __FILE__, __LINE__);
        return -1;
    }
    info->product = product;
    info->swath_name = swath_name;

    if (init_cursors(info) != 0)
    {
        ingestion_done(info);
        return -1;
    }
    if (get_dimensions(info) != 0)
    {
        ingestion_done(info);
        return -1;
    }

    *definition = *module->product_definition;
    *user_data = info;

    return 0;
}

static int ingestion_init_bro(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "BrO");
}

static int ingestion_init_clo(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "ClO");
}

static int ingestion_init_co(const harp_ingestion_module *module, coda_product *product,
                             const harp_ingestion_options *options, harp_product_definition **definition,
                             void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "CO");
}

static int ingestion_init_gph(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "GPH");
}

static int ingestion_init_h2o(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "H2O");
}

static int ingestion_init_hcl(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "HCl");
}

static int ingestion_init_hcn(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "HCN");
}

static int ingestion_init_hno3(const harp_ingestion_module *module, coda_product *product,
                               const harp_ingestion_options *options, harp_product_definition **definition,
                               void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "HNO3");
}

static int ingestion_init_ho2(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "HO2");
}

static int ingestion_init_hocl(const harp_ingestion_module *module, coda_product *product,
                               const harp_ingestion_options *options, harp_product_definition **definition,
                               void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "HOCl");
}

static int ingestion_init_iwc(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "IWC");
}

static int ingestion_init_n2o(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "N2O");
}

static int ingestion_init_o3(const harp_ingestion_module *module, coda_product *product,
                             const harp_ingestion_options *options, harp_product_definition **definition,
                             void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "O3");
}

static int ingestion_init_oh(const harp_ingestion_module *module, coda_product *product,
                             const harp_ingestion_options *options, harp_product_definition **definition,
                             void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "OH");
}

static int ingestion_init_rhi(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "RHI");
}

static int ingestion_init_so2(const harp_ingestion_module *module, coda_product *product,
                              const harp_ingestion_options *options, harp_product_definition **definition,
                              void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "SO2");
}

static int ingestion_init_t(const harp_ingestion_module *module, coda_product *product,
                            const harp_ingestion_options *options, harp_product_definition **definition,
                            void **user_data)
{
    return ingestion_init(module, product, options, definition, user_data, "Temperature");
}

static int verify_product_type(coda_product *product, const char *swath_name)
{
    coda_cursor cursor;
    char buffer[100];
    long string_length;

    if (coda_cursor_set_product(&cursor, product) != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }
    if (coda_cursor_goto(&cursor, "/HDFEOS/ADDITIONAL/FILE_ATTRIBUTES@InstrumentName") != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }
    if (coda_cursor_get_string_length(&cursor, &string_length) != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }
    if (string_length != 8)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }
    if (coda_cursor_read_string(&cursor, buffer, 9) != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }
    if (strcmp(buffer, "MLS Aura") != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }

    if (coda_cursor_goto(&cursor, "../ProcessLevel") != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }
    if (coda_cursor_get_string_length(&cursor, &string_length) != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }
    if (string_length > 99)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }
    if (coda_cursor_read_string(&cursor, buffer, 100) != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }
    if (strncmp(buffer, "2", 1) != 0 && strncmp(buffer, "L2", 2) != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }

    if (coda_cursor_goto(&cursor, "/HDFEOS/SWATHS") != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }
    if (coda_cursor_goto_record_field_by_name(&cursor, swath_name) != 0)
    {
        harp_set_error(HARP_ERROR_UNSUPPORTED_PRODUCT, NULL);
        return -1;
    }

    return 0;
}

static int verify_bro(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "BrO");
}

static int verify_clo(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "ClO");
}

static int verify_co(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "CO");
}

static int verify_gph(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "GPH");
}

static int verify_h2o(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "H2O");
}

static int verify_hcl(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "HCl");
}

static int verify_hcn(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "HCN");
}

static int verify_hno3(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "HNO3");
}

static int verify_ho2(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "HO2");
}

static int verify_hocl(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "HOCl");
}

static int verify_iwc(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "IWC");
}

static int verify_n2o(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "N2O");
}

static int verify_o3(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "O3");
}

static int verify_oh(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "OH");
}

static int verify_rhi(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "RHI");
}

static int verify_so2(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "SO2");
}

static int verify_t(const harp_ingestion_module *module, coda_product *product)
{
    (void)module;
    return verify_product_type(product, "Temperature");
}

static void register_datetime_variable(harp_product_definition *product_definition, const char *path)
{
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[1] = { harp_dimension_time };
    const char *description;

    description = "time of the measurement (in seconds since 2000-01-01 00:00:00)";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "datetime", harp_type_double, 1, dimension_type,
                                                   NULL, description, "seconds since 2000-01-01", NULL, read_time);

    description = "the time converted from TAI93 to seconds since 2000-01-01T00:00:00";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, description);
}

static void register_longitude_variable(harp_product_definition *product_definition, const char *path)
{
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[1] = { harp_dimension_time };
    const char *description;

    description = "tangent longitude";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "longitude", harp_type_double, 1, dimension_type,
                                                   NULL, description, "degree_east", NULL, read_longitude);
    harp_variable_definition_set_valid_range_double(variable_definition, -180.0, 180.0);
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_latitude_variable(harp_product_definition *product_definition, const char *path)
{
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[1] = { harp_dimension_time };
    const char *description;

    description = "tangent latitude";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "latitude", harp_type_double, 1, dimension_type,
                                                   NULL, description, "degree_north", NULL, read_latitude);
    harp_variable_definition_set_valid_range_double(variable_definition, -90.0, 90.0);
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_pressure_variable(harp_product_definition *product_definition, const char *path)
{
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[1] = { harp_dimension_vertical };
    const char *description;

    description = "pressure per profile level";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "pressure", harp_type_double, 1, dimension_type,
                                                   NULL, description, "hPa", NULL, read_pressure);
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_bro_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_BRO", NULL, NULL, "MLS BrO profile", verify_bro, ingestion_init_bro,
                                            ingestion_done);

    /* BRO product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_BRO", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/BrO/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/BrO/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/BrO/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/BrO/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* BrO_volume_mixing_ratio */
    description = "BrO volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "BrO_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/BrO/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* BrO_volume_mixing_ratio_stdev */
    description = "uncertainty of the BrO volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "BrO_volume_mixing_ratio_stdev",
                                                   harp_type_double, 2, dimension_type, NULL, description, "ppv", NULL,
                                                   read_error);
    path = "/HDFEOS/SWATHS/BrO/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_clo_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_CLO", NULL, NULL, "MLS ClO profile", verify_clo, ingestion_init_clo,
                                            ingestion_done);

    /* CLO product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_CLO", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/ClO/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/ClO/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/ClO/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/ClO/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* ClO_volume_mixing_ratio */
    description = "ClO volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "ClO_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/ClO/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* ClO_volume_mixing_ratio_stdev */
    description = "uncertainty of the ClO volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "ClO_volume_mixing_ratio_stdev",
                                                   harp_type_double, 2, dimension_type, NULL, description, "ppv", NULL,
                                                   read_error);
    path = "/HDFEOS/SWATHS/ClO/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_co_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_CO", NULL, NULL, "MLS CO profile", verify_co, ingestion_init_co,
                                            ingestion_done);

    /* CO product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_CO", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/CO/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/CO/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/CO/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/CO/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* CO_volume_mixing_ratio */
    description = "CO volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "CO_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/CO/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* CO_volume_mixing_ratio_stdev */
    description = "uncertainty of the CO volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "CO_volume_mixing_ratio_stdev", harp_type_double,
                                                   2, dimension_type, NULL, description, "ppv", NULL, read_error);
    path = "/HDFEOS/SWATHS/CO/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_gph_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_GPH", NULL, NULL, "MLS GPH profile", verify_gph, ingestion_init_gph,
                                            ingestion_done);

    /* GPH product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_GPH", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/GPH/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/GPH/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/GPH/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/GPH/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* altitude */
    description = "retrieved altitude";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "altitude", harp_type_double, 2, dimension_type,
                                                   NULL, description, "m", NULL, read_value);
    path = "/HDFEOS/SWATHS/GPH/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* altitude_stdev */
    description = "uncertainty of the retrieved altitude";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "altitude_stdev", harp_type_double, 2,
                                                   dimension_type, NULL, description, "m", NULL, read_error);
    path = "/HDFEOS/SWATHS/GPH/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_h2o_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_H2O", NULL, NULL, "MLS H2O profile", verify_h2o, ingestion_init_h2o,
                                            ingestion_done);

    /* H2O product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_H2O", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/H2O/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/H2O/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/H2O/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/H2O/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* H2O_volume_mixing_ratio */
    description = "H2O volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "H2O_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/H2O/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* H2O_volume_mixing_ratio_stdev */
    description = "uncertainty of the H2O volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "H2O_volume_mixing_ratio_stdev",
                                                   harp_type_double, 2, dimension_type, NULL, description, "ppv", NULL,
                                                   read_error);
    path = "/HDFEOS/SWATHS/H2O/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_hcl_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_HCL", NULL, NULL, "MLS HCl profile", verify_hcl, ingestion_init_hcl,
                                            ingestion_done);

    /* HCL product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_HCL", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/HCL/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/HCL/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/HCL/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/HCL/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* HCl_volume_mixing_ratio */
    description = "HCl volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "HCl_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/HCL/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* HCl_volume_mixing_ratio_stdev */
    description = "uncertainty of the HCl volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "HCl_volume_mixing_ratio_stdev",
                                                   harp_type_double, 2, dimension_type, NULL, description, "ppv", NULL,
                                                   read_error);
    path = "/HDFEOS/SWATHS/HCL/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_hcn_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_HCN", NULL, NULL, "MLS HCN profile", verify_hcn, ingestion_init_hcn,
                                            ingestion_done);

    /* HCN product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_HCN", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/HCN/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/HCN/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/HCN/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/HCN/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* HCN_volume_mixing_ratio */
    description = "HCN volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "HCN_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/HCN/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* HCN_volume_mixing_ratio_stdev */
    description = "uncertainty of the HCN volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "HCN_volume_mixing_ratio_stdev",
                                                   harp_type_double, 2, dimension_type, NULL, description, "ppv", NULL,
                                                   read_error);
    path = "/HDFEOS/SWATHS/HCN/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_hno3_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_HNO3", NULL, NULL, "MLS HNO3 profile", verify_hno3,
                                            ingestion_init_hno3, ingestion_done);

    /* HNO3 product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_HNO3", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/HNO3/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/HNO3/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/HNO3/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/HNO3/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* HNO3_volume_mixing_ratio */
    description = "HNO3 volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "HNO3_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/HNO3/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* HNO3_volume_mixing_ratio_stdev */
    description = "uncertainty of the HNO3 volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "HNO3_volume_mixing_ratio_stdev",
                                                   harp_type_double, 2, dimension_type, NULL, description, "ppv", NULL,
                                                   read_error);
    path = "/HDFEOS/SWATHS/HNO3/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_ho2_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_HO2", NULL, NULL, "MLS HO2 profile", verify_ho2, ingestion_init_ho2,
                                            ingestion_done);

    /* HO2 product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_HO2", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/HO2/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/HO2/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/HO2/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/HO2/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* HO2_volume_mixing_ratio */
    description = "HO2 volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "HO2_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/HO2/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* HO2_volume_mixing_ratio_stdev */
    description = "uncertainty of the HO2 volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "HO2_volume_mixing_ratio_stdev",
                                                   harp_type_double, 2, dimension_type, NULL, description, "ppv", NULL,
                                                   read_error);
    path = "/HDFEOS/SWATHS/HO2/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_hocl_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_HOCL", NULL, NULL, "MLS HOCl profile", verify_hocl,
                                            ingestion_init_hocl, ingestion_done);

    /* HOCL product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_HOCL", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/HOCL/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/HOCL/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/HOCL/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/HOCL/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* HOCl_volume_mixing_ratio */
    description = "HOCl volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "HOCl_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/HOCL/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* HOCl_volume_mixing_ratio_stdev */
    description = "uncertainty of the HOCl volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "HOCl_volume_mixing_ratio_stdev",
                                                   harp_type_double, 2, dimension_type, NULL, description, "ppv", NULL,
                                                   read_error);
    path = "/HDFEOS/SWATHS/HOCL/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_iwc_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_IWC", NULL, NULL, "MLS ice water content profile", verify_iwc,
                                            ingestion_init_iwc, ingestion_done);

    /* IWC product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_IWC", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/IWC/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/IWC/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/IWC/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/IWC/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* ice_water_content */
    description = "Ice water content";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "ice_water_content", harp_type_double, 2,
                                                   dimension_type, NULL, description, "g/m^3", NULL, read_value);
    path = "/HDFEOS/SWATHS/IWC/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* ice_water_content_stdev */
    description = "uncertainty of the ice water content";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "ice_water_content_stdev", harp_type_double, 2,
                                                   dimension_type, NULL, description, "g/m^3", NULL, read_value);
    path = "/HDFEOS/SWATHS/IWC/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_n2o_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_N2O", NULL, NULL, "MLS N2O profile", verify_n2o, ingestion_init_n2o,
                                            ingestion_done);

    /* N2O product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_N2O", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/N2O/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/N2O/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/N2O/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/N2O/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* N2O_volume_mixing_ratio */
    description = "N2O volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "N2O_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/N2O/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* N2O_volume_mixing_ratio_stdev */
    description = "uncertainty of the N2O volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "N2O_volume_mixing_ratio_stdev",
                                                   harp_type_double, 2, dimension_type, NULL, description, "ppv", NULL,
                                                   read_error);
    path = "/HDFEOS/SWATHS/N2O/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_o3_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_O3", NULL, NULL, "MLS O3 profile", verify_o3, ingestion_init_o3,
                                            ingestion_done);

    /* O3 product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_O3", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/O3/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/O3/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/O3/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/O3/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* O3_volume_mixing_ratio */
    description = "O3 volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "O3_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/O3/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* O3_volume_mixing_ratio_stdev */
    description = "uncertainty of the O3 volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "O3_volume_mixing_ratio_stdev", harp_type_double,
                                                   2, dimension_type, NULL, description, "ppv", NULL, read_error);
    path = "/HDFEOS/SWATHS/O3/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_oh_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_OH", NULL, NULL, "MLS OH profile", verify_oh, ingestion_init_oh,
                                            ingestion_done);

    /* OH product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_OH", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/OH/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/OH/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/OH/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/OH/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* OH_volume_mixing_ratio */
    description = "OH volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "OH_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/OH/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* OH_volume_mixing_ratio_stdev */
    description = "uncertainty of the OH volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "OH_volume_mixing_ratio_stdev", harp_type_double,
                                                   2, dimension_type, NULL, description, "ppv", NULL, read_error);
    path = "/HDFEOS/SWATHS/OH/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_rhi_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_RHI", NULL, NULL,
                                            "MLS relative humidity with respect to ice profile", verify_rhi,
                                            ingestion_init_rhi, ingestion_done);

    /* RHI product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_RHI", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/RHI/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/RHI/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/RHI/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/RHI/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* relative_humidity_ice */
    description = "relative humidity with respect to ice";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "relative_humidity_ice", harp_type_double, 2,
                                                   dimension_type, NULL, description, "%", NULL, read_value);
    path = "/HDFEOS/SWATHS/RHI/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* relative_humidity_ice_stdev */
    description = "uncertainty of the relative humidity with respect to ice";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "relative_humidity_ice_stdev", harp_type_double,
                                                   2, dimension_type, NULL, description, "%", NULL, read_error);
    path = "/HDFEOS/SWATHS/RHI/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_so2_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_SO2", NULL, NULL, "MLS SO2 profile", verify_so2, ingestion_init_so2,
                                            ingestion_done);

    /* SO2 product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_SO2", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/SO2/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/SO2/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/SO2/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/SO2/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* SO2_volume_mixing_ratio */
    description = "SO2 volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "SO2_volume_mixing_ratio", harp_type_double, 2,
                                                   dimension_type, NULL, description, "ppv", NULL, read_value);
    path = "/HDFEOS/SWATHS/SO2/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* SO2_volume_mixing_ratio_stdev */
    description = "uncertainty of the SO2 volume mixing ratio";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "SO2_volume_mixing_ratio_stdev",
                                                   harp_type_double, 2, dimension_type, NULL, description, "ppv", NULL,
                                                   read_error);
    path = "/HDFEOS/SWATHS/SO2/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

static void register_t_product(void)
{
    harp_ingestion_module *module;
    harp_product_definition *product_definition;
    harp_variable_definition *variable_definition;
    harp_dimension_type dimension_type[2] = { harp_dimension_time, harp_dimension_vertical };
    const char *description;
    const char *path;

    module =
        harp_ingestion_register_module_coda("MLS_L2_T", NULL, NULL, "MLS temperature profile", verify_t,
                                            ingestion_init_t, ingestion_done);

    /* T product */
    product_definition = harp_ingestion_register_product(module, "MLS_L2_T", NULL, read_dimensions);

    /* datetime */
    path = "/HDFEOS/SWATHS/Temperature/Geolocation_Fields/Time[]";
    register_datetime_variable(product_definition, path);

    /* longitude and latitude */
    path = "/HDFEOS/SWATHS/Temperature/Geolocation_Fields/Longitude[]";
    register_longitude_variable(product_definition, path);
    path = "/HDFEOS/SWATHS/Temperature/Geolocation_Fields/Latitude[]";
    register_latitude_variable(product_definition, path);

    /* pressure */
    path = "/HDFEOS/SWATHS/Temperature/Geolocation_Fields/Pressure[]";
    register_pressure_variable(product_definition, path);

    /* temperature */
    description = "temperature";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "temperature", harp_type_double, 2,
                                                   dimension_type, NULL, description, "K", NULL, read_value);
    path = "/HDFEOS/SWATHS/Temperature/Data_Fields/L2gpValue[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);

    /* temperature_stdev */
    description = "uncertainty of the temperature";
    variable_definition =
        harp_ingestion_register_variable_full_read(product_definition, "temperature_stdev", harp_type_double, 2,
                                                   dimension_type, NULL, description, "K", NULL, read_error);
    path = "/HDFEOS/SWATHS/Temperature/Data_Fields/L2gpPrecision[]";
    harp_variable_definition_add_mapping(variable_definition, NULL, NULL, path, NULL);
}

int harp_ingestion_module_mls_l2_init(void)
{
    register_bro_product();
    register_clo_product();
    register_co_product();
    register_gph_product();
    register_h2o_product();
    register_hcl_product();
    register_hcn_product();
    register_hno3_product();
    register_ho2_product();
    register_hocl_product();
    register_iwc_product();
    register_n2o_product();
    register_o3_product();
    register_oh_product();
    register_rhi_product();
    register_so2_product();
    register_t_product();

    return 0;
}