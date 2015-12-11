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

#include "hashtable.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct conversion_info_struct
{
    const harp_product *product;
    const harp_variable_conversion *conversion;
    const char *variable_name;
    int num_dimensions;
    harp_dimension_type dimension_type[HARP_MAX_NUM_DIMS];
    uint8_t *skip;      /* bit mask where 2^num_dims is set to skip variable with that many dimensions */
    harp_variable *variable;
} conversion_info;

static int find_and_execute_conversion(conversion_info *info);

static int has_dimension_types(const harp_variable *variable, int num_dimensions,
                               const harp_dimension_type *dimension_type, long independent_dimension_length)
{
    int i;

    if (variable->num_dimensions != num_dimensions)
    {
        return 0;
    }

    for (i = 0; i < num_dimensions; i++)
    {
        if (variable->dimension_type[i] != dimension_type[i])
        {
            return 0;
        }
        if (dimension_type[i] == harp_dimension_independent && independent_dimension_length >= 0 &&
            variable->dimension[i] != independent_dimension_length)
        {
            return 0;
        }
    }

    return 1;
}

static int create_variable(conversion_info *info)
{
    const harp_variable_conversion *conversion = info->conversion;
    long dimension[HARP_MAX_NUM_DIMS];
    harp_variable *variable = NULL;
    int i;

    for (i = 0; i < conversion->num_dimensions; i++)
    {
        if (conversion->dimension_type[i] == harp_dimension_independent)
        {
            dimension[i] = conversion->independent_dimension_length;
        }
        else
        {
            dimension[i] = info->product->dimension[conversion->dimension_type[i]];
        }
    }

    if (harp_variable_new(conversion->variable_name, conversion->data_type, conversion->num_dimensions,
                          conversion->dimension_type, dimension, &variable) != 0)
    {
        return -1;
    }

    /* Set unit */
    if (conversion->unit != NULL)
    {
        variable->unit = strdup(conversion->unit);
        if (variable->unit == NULL)
        {
            harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not duplicate string) (%s:%u)", __FILE__,
                           __LINE__);
            harp_variable_delete(variable);
            return -1;
        }
    }

    info->variable = variable;

    return 0;
}

static int get_source_variable(conversion_info *info, harp_data_type data_type, const char *unit, int *is_temp)
{
    *is_temp = 0;

    if (harp_product_get_variable_by_name(info->product, info->variable_name, &info->variable) == 0)
    {
        if (harp_variable_has_dimension_types(info->variable, info->num_dimensions, info->dimension_type))
        {
            /* variable already exists */
            if (unit != NULL && !harp_variable_has_unit(info->variable, unit))
            {
                /* create a copy if we need to perform unit conversion */
                if (harp_variable_copy(info->variable, &info->variable) != 0)
                {
                    return -1;
                }
                *is_temp = 1;
                if (harp_variable_convert_unit(info->variable, unit) != 0)
                {
                    harp_variable_delete(info->variable);
                    info->variable = NULL;
                    return -1;
                }
            }
            if (info->variable->data_type != data_type)
            {
                if (*is_temp == 0)
                {
                    /* create a copy if we need to perform data type conversion */
                    if (harp_variable_copy(info->variable, &info->variable) != 0)
                    {
                        return -1;
                    }
                    *is_temp = 1;
                }
                if (harp_variable_convert_data_type(info->variable, data_type) != 0)
                {
                    harp_variable_delete(info->variable);
                    info->variable = NULL;
                    return -1;
                }
            }
            return 0;
        }
    }

    *is_temp = 1;

    if (find_and_execute_conversion(info) != 0)
    {
        return -1;
    }

    if (unit != NULL)
    {
        if (harp_variable_convert_unit(info->variable, unit) != 0)
        {
            harp_variable_delete(info->variable);
            info->variable = NULL;
            return -1;
        }
    }

    return 0;
}

static int perform_conversion(conversion_info *info)
{
    harp_variable *source_variable[MAX_NUM_SOURCE_VARIABLES];
    int is_temp[MAX_NUM_SOURCE_VARIABLES];
    int result;
    int i, j;

    for (i = 0; i < info->conversion->num_source_variables; i++)
    {
        conversion_info source_info = *info;
        harp_source_variable_definition *source_definition = &info->conversion->source_definition[i];

        source_info.conversion = NULL;
        source_info.variable_name = source_definition->variable_name;
        source_info.num_dimensions = source_definition->num_dimensions;
        for (j = 0; j < source_info.num_dimensions; j++)
        {
            source_info.dimension_type[j] = source_definition->dimension_type[j];
        }
        source_info.variable = NULL;
        if (get_source_variable(&source_info, source_definition->data_type, source_definition->unit, &is_temp[i]) != 0)
        {
            for (j = 0; j < i; j++)
            {
                if (is_temp[j])
                {
                    harp_variable_delete(source_variable[j]);
                }
            }
            return -1;
        }
        source_variable[i] = source_info.variable;
    }

    result = create_variable(info);
    if (result == 0)
    {
        result = info->conversion->set_variable_data(info->variable, (const harp_variable **)source_variable);
        if (result != 0)
        {
            harp_variable_delete(info->variable);
            info->variable = NULL;
        }
        /* TODO: set description of variable based on the applied conversion
         * e.g. <target_var_name> from (<source_var_name> from ...), (<source_var_2_name> from ...)
         */
    }

    for (j = 0; j < info->conversion->num_source_variables; j++)
    {
        if (is_temp[j])
        {
            harp_variable_delete(source_variable[j]);
        }
    }

    return result;
}

static int find_source_variable(conversion_info *info, harp_source_variable_definition *source_definition)
{
    harp_variable *variable;
    int index;

    if (harp_product_get_variable_by_name(info->product, source_definition->variable_name, &variable) == 0)
    {
        if (has_dimension_types(variable, source_definition->num_dimensions, source_definition->dimension_type,
                                source_definition->independent_dimension_length))
        {
            /* variable is present in the product */
            return 1;
        }
    }

    /* try to find a conversion for the variable */
    index = hashtable_get_index_from_name(harp_derived_variable_conversions->hash_data,
                                          source_definition->variable_name);
    if (index >= 0)
    {
        harp_variable_conversion_list *conversion_list;
        int i;

        conversion_list = harp_derived_variable_conversions->conversions_for_variable[index];
        for (i = 0; i < conversion_list->num_conversions; i++)
        {
            harp_variable_conversion *conversion = conversion_list->conversion[i];
            int j;

            if (conversion->enabled != NULL && !conversion->enabled())
            {
                continue;
            }
            if (info->skip[index] & 1 << conversion->num_dimensions)
            {
                continue;
            }
            /* check if conversion provides the right dimension types */
            if (conversion->num_dimensions != source_definition->num_dimensions)
            {
                continue;
            }
            for (j = 0; j < conversion->num_dimensions; j++)
            {
                if (conversion->dimension_type[j] != source_definition->dimension_type[j])
                {
                    break;
                }
                if (conversion->dimension_type[j] == harp_dimension_independent &&
                    source_definition->independent_dimension_length >= 0 &&
                    conversion->independent_dimension_length != source_definition->independent_dimension_length)
                {
                    break;
                }
            }
            if (j < conversion->num_dimensions)
            {
                continue;
            }

            info->skip[index] ^= 1 << conversion->num_dimensions;

            for (j = 0; j < conversion->num_source_variables; j++)
            {
                int result;

                /* recursively find the source variables for creating this variable */
                result = find_source_variable(info, &conversion->source_definition[j]);
                if (result == -1)
                {
                    info->skip[index] ^= 1 << conversion->num_dimensions;
                    return -1;
                }
                if (result == 0)
                {
                    /* source not found */
                    break;
                }
            }

            info->skip[index] ^= 1 << conversion->num_dimensions;

            if (j == conversion->num_source_variables)
            {
                /* the conversion is possible */
                return 1;
            }
        }
    }

    /* no conversion found */
    return 0;
}

static int find_and_execute_conversion(conversion_info *info)
{
    int index;

    index = hashtable_get_index_from_name(harp_derived_variable_conversions->hash_data, info->variable_name);
    if (index >= 0)
    {
        harp_variable_conversion_list *conversion_list =
            harp_derived_variable_conversions->conversions_for_variable[index];
        int i;

        for (i = 0; i < conversion_list->num_conversions; i++)
        {
            harp_variable_conversion *conversion = conversion_list->conversion[i];
            int j;

            if (conversion->enabled != NULL && !conversion->enabled())
            {
                continue;
            }
            if (info->skip[index] & 1 << conversion->num_dimensions)
            {
                continue;
            }

            /* check if conversion has the right dimensions */
            if (conversion->num_dimensions != info->num_dimensions)
            {
                continue;
            }
            for (j = 0; j < conversion->num_dimensions; j++)
            {
                if (conversion->dimension_type[j] != info->dimension_type[j])
                {
                    break;
                }
            }
            if (j < conversion->num_dimensions)
            {
                continue;
            }

            info->skip[index] ^= 1 << conversion->num_dimensions;

            for (j = 0; j < conversion->num_source_variables; j++)
            {
                int result;

                result = find_source_variable(info, &conversion->source_definition[j]);
                if (result == -1)
                {
                    info->skip[index] ^= 1 << conversion->num_dimensions;
                    return -1;
                }
                if (result == 0)
                {
                    /* source not found */
                    break;
                }
            }

            if (j == conversion->num_source_variables)
            {
                int result;

                /* conversion should be possible */
                info->conversion = conversion;
                result = perform_conversion(info);
                info->skip[index] ^= 1 << conversion->num_dimensions;
                return result;
            }

            info->skip[index] ^= 1 << conversion->num_dimensions;
        }
    }

    harp_set_error(HARP_ERROR_VARIABLE_NOT_FOUND, "could not derive variable '%s'", info->variable_name);
    return -1;
}

static void print_conversion(conversion_info *info, int indent);

static int find_and_print_conversion(conversion_info *info, int indent)
{
    int index;

    index = hashtable_get_index_from_name(harp_derived_variable_conversions->hash_data, info->variable_name);
    if (index >= 0)
    {
        harp_variable_conversion_list *conversion_list =
            harp_derived_variable_conversions->conversions_for_variable[index];
        int i;

        for (i = 0; i < conversion_list->num_conversions; i++)
        {
            harp_variable_conversion *conversion = conversion_list->conversion[i];
            int j;

            if (conversion->enabled != NULL && !conversion->enabled())
            {
                continue;
            }
            if (info->skip[index] & 1 << conversion->num_dimensions)
            {
                continue;
            }

            /* check if conversion has the right dimensions */
            if (conversion->num_dimensions != info->num_dimensions)
            {
                continue;
            }
            for (j = 0; j < conversion->num_dimensions; j++)
            {
                if (conversion->dimension_type[j] != info->dimension_type[j])
                {
                    break;
                }
            }
            if (j < conversion->num_dimensions)
            {
                continue;
            }

            info->skip[index] ^= 1 << conversion->num_dimensions;

            for (j = 0; j < conversion->num_source_variables; j++)
            {
                int result;

                result = find_source_variable(info, &conversion->source_definition[j]);
                if (result == -1)
                {
                    info->skip[index] ^= 1 << conversion->num_dimensions;
                    return -1;
                }
                if (result == 0)
                {
                    /* source not found */
                    break;
                }
            }

            if (j == conversion->num_source_variables)
            {
                /* all source variables were found, conversion should be possible */
                info->conversion = conversion;
                print_conversion(info, indent + 1);
                info->skip[index] ^= 1 << conversion->num_dimensions;
                return 0;
            }

            info->skip[index] ^= 1 << conversion->num_dimensions;
        }
    }

    harp_set_error(HARP_ERROR_VARIABLE_NOT_FOUND, "could not derive variable '%s'", info->variable_name);
    return -1;
}

static int print_source_variable_conversion(conversion_info *info, int indent)
{
    if (harp_product_get_variable_by_name(info->product, info->variable_name, &info->variable) == 0)
    {
        if (harp_variable_has_dimension_types(info->variable, info->num_dimensions, info->dimension_type))
        {
            printf("\n");
            return 0;
        }
    }
    return find_and_print_conversion(info, indent);
}

static void print_conversion_variable(const harp_variable_conversion *conversion)
{
    int i;

    printf("%s", conversion->variable_name);
    if (conversion->num_dimensions > 0)
    {
        printf(" {");
        for (i = 0; i < conversion->num_dimensions; i++)
        {
            printf("%s", harp_get_dimension_type_name(conversion->dimension_type[i]));
            if (conversion->dimension_type[i] == harp_dimension_independent)
            {
                printf("(%ld)", conversion->independent_dimension_length);
            }
            if (i < conversion->num_dimensions - 1)
            {
                printf(",");
            }
        }
        printf("}");
    }
    if (conversion->unit != NULL)
    {
        printf(" [%s]", conversion->unit);
    }
    printf(" (%s)", harp_get_data_type_name(conversion->data_type));
}

static void print_source_variable(const harp_source_variable_definition *source_definition, int indent)
{
    int k;

    for (k = 0; k < indent; k++)
    {
        printf("  ");
    }
    printf("%s", source_definition->variable_name);
    if (source_definition->num_dimensions > 0)
    {
        printf(" {");
        for (k = 0; k < source_definition->num_dimensions; k++)
        {
            printf("%s", harp_get_dimension_type_name(source_definition->dimension_type[k]));
            if (source_definition->dimension_type[k] == harp_dimension_independent &&
                source_definition->independent_dimension_length >= 0)
            {
                printf("(%ld)", source_definition->independent_dimension_length);
            }
            if (k < source_definition->num_dimensions - 1)
            {
                printf(",");
            }
        }
        printf("}");
    }
    if (source_definition->unit != NULL)
    {
        printf(" [%s]", source_definition->unit);
    }
    printf(" (%s)", harp_get_data_type_name(source_definition->data_type));
}

static void print_conversion(conversion_info *info, int indent)
{
    int i, k;

    if (info->conversion->num_source_variables == 0)
    {
        printf("\n");
        for (k = 0; k < indent; k++)
        {
            printf("  ");
        }
        printf("derived without input variables\n");
    }
    else
    {
        printf(" from\n");
        for (i = 0; i < info->conversion->num_source_variables; i++)
        {
            conversion_info source_info = *info;
            harp_source_variable_definition *source_definition = &info->conversion->source_definition[i];

            print_source_variable(source_definition, indent);
            source_info.conversion = NULL;
            source_info.variable_name = source_definition->variable_name;
            source_info.num_dimensions = source_definition->num_dimensions;
            for (k = 0; k < source_info.num_dimensions; k++)
            {
                source_info.dimension_type[k] = source_definition->dimension_type[k];
            }
            source_info.variable = NULL;
            if (print_source_variable_conversion(&source_info, indent) != 0)
            {
                for (k = 0; k < indent; k++)
                {
                    printf("  ");
                }
                printf("ERROR: %s\n", harp_errno_to_string(harp_errno));
            }
        }
    }
    if (info->conversion->source_description != NULL)
    {
        for (k = 0; k < indent; k++)
        {
            printf("  ");
        }
        printf("note: %s\n", info->conversion->source_description);
    }
}

void harp_variable_conversion_print(const harp_variable_conversion *conversion)
{
    int i;

    print_conversion_variable(conversion);
    if (conversion->num_source_variables > 0)
    {
        printf(" from\n");
        for (i = 0; i < conversion->num_source_variables; i++)
        {
            print_source_variable(&conversion->source_definition[i], 1);
            printf("\n");
        }
    }
    else
    {
        printf("\n  derived without input variables\n");
    }
    if (conversion->source_description != NULL)
    {
        printf("  note: %s\n", conversion->source_description);
    }
    printf("\n");
}

void harp_variable_conversion_delete(harp_variable_conversion *conversion)
{
    if (conversion == NULL)
    {
        return;
    }

    if (conversion->variable_name != NULL)
    {
        free(conversion->variable_name);
    }
    if (conversion->unit != NULL)
    {
        free(conversion->unit);
    }

    if (conversion->source_definition != NULL)
    {
        int i;

        for (i = 0; i < conversion->num_source_variables; i++)
        {
            if (conversion->source_definition[i].variable_name != NULL)
            {
                free(conversion->source_definition[i].variable_name);
            }
            if (conversion->source_definition[i].unit != NULL)
            {
                free(conversion->source_definition[i].unit);
            }
        }
        free(conversion->source_definition);
    }

    if (conversion->source_description != NULL)
    {
        free(conversion->source_description);
    }

    free(conversion);
}

/* this function also adds the conversion to the global derived variable conversion list */
int harp_variable_conversion_new(const char *variable_name, harp_data_type data_type, const char *unit,
                                 int num_dimensions, harp_dimension_type *dimension_type,
                                 long independent_dimension_length, harp_conversion_function set_variable_data,
                                 harp_variable_conversion **new_conversion)
{
    harp_variable_conversion *conversion = NULL;
    int i;

    conversion = (harp_variable_conversion *)malloc(sizeof(harp_variable_conversion));
    if (conversion == NULL)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not allocate % lu bytes) (%s:%u)",
                       sizeof(harp_variable_conversion), __FILE__, __LINE__);
        return -1;
    }
    conversion->variable_name = NULL;
    conversion->data_type = data_type;
    conversion->unit = NULL;
    conversion->num_dimensions = num_dimensions;
    for (i = 0; i < num_dimensions; i++)
    {
        conversion->dimension_type[i] = dimension_type[i];
    }
    conversion->independent_dimension_length = independent_dimension_length;
    conversion->num_source_variables = 0;
    conversion->source_definition = NULL;
    conversion->source_description = NULL;
    conversion->set_variable_data = set_variable_data;
    conversion->enabled = NULL;

    conversion->variable_name = strdup(variable_name);
    if (conversion->variable_name == NULL)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not duplicate string) (%s:%u)", __FILE__,
                       __LINE__);
        harp_variable_conversion_delete(conversion);
        return -1;
    }

    if (unit != NULL)
    {
        conversion->unit = strdup(unit);
        if (conversion->unit == NULL)
        {
            harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not duplicate string) (%s:%u)", __FILE__,
                           __LINE__);
            harp_variable_conversion_delete(conversion);
            return -1;
        }
    }

    if (harp_derived_variable_list_add_conversion(conversion) != 0)
    {
        harp_variable_conversion_delete(conversion);
        return -1;
    }

    *new_conversion = conversion;

    return 0;
}

int harp_variable_conversion_add_source(harp_variable_conversion *conversion, const char *variable_name,
                                        harp_data_type data_type, const char *unit, int num_dimensions,
                                        harp_dimension_type *dimension_type, long independent_dimension_length)
{
    harp_source_variable_definition *source_definition;
    int i;

    assert(conversion->num_source_variables < MAX_NUM_SOURCE_VARIABLES);

    source_definition = realloc(conversion->source_definition,
                                (conversion->num_source_variables + 1) * sizeof(harp_source_variable_definition));
    if (source_definition == NULL)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory(could not allocate % lu bytes) (%s:%u)",
                       (conversion->num_source_variables + 1) * sizeof(harp_source_variable_definition),
                       __FILE__, __LINE__);
        return -1;
    }
    conversion->source_definition = source_definition;

    source_definition = &conversion->source_definition[conversion->num_source_variables];
    conversion->num_source_variables++;

    source_definition->variable_name = NULL;
    source_definition->data_type = data_type;
    source_definition->unit = NULL;
    source_definition->num_dimensions = num_dimensions;
    source_definition->independent_dimension_length = independent_dimension_length;
    for (i = 0; i < num_dimensions; i++)
    {
        source_definition->dimension_type[i] = dimension_type[i];
    }

    source_definition->variable_name = strdup(variable_name);
    if (source_definition->variable_name == NULL)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not duplicate string) (%s:%u)", __FILE__,
                       __LINE__);
        return -1;
    }

    if (unit != NULL)
    {
        source_definition->unit = strdup(unit);
        if (source_definition->unit == NULL)
        {
            harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not duplicate string) (%s:%u)", __FILE__,
                           __LINE__);
            return -1;
        }
    }

    return 0;
}

int harp_variable_conversion_set_enabled_function(harp_variable_conversion *conversion,
                                                  harp_conversion_enabled_function enabled)
{
    assert(conversion->enabled == NULL);

    conversion->enabled = enabled;

    return 0;
}

int harp_variable_conversion_set_source_description(harp_variable_conversion *conversion, const char *description)
{
    assert(conversion->source_description == NULL);

    conversion->source_description = strdup(description);
    if (conversion->source_description == NULL)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not duplicate string) (%s:%u)", __FILE__,
                       __LINE__);
        return -1;
    }

    return 0;
}

int harp_list_conversions(const harp_product *product)
{
    conversion_info info;
    int i, j;

    if (harp_derived_variable_conversions == NULL)
    {
        if (harp_derived_variable_list_init() != 0)
        {
            return -1;
        }
    }

    if (product == NULL)
    {
        /* just print all conversions */
        for (i = 0; i < harp_derived_variable_conversions->num_variables; i++)
        {
            harp_variable_conversion_list *conversion_list =
                harp_derived_variable_conversions->conversions_for_variable[i];
            int first = 1;

            for (j = 0; j < conversion_list->num_conversions; j++)
            {
                harp_variable_conversion *conversion = conversion_list->conversion[j];

                if (first)
                {
                    printf("============================================================\n");
                    first = 0;
                }

                if (conversion->enabled != NULL && !conversion->enabled())
                {
                    continue;
                }
                harp_variable_conversion_print(conversion);
            }
        }
        return 0;
    }

    info.product = product;
    info.conversion = NULL;
    info.skip = NULL;
    info.variable = NULL;

    info.skip = malloc(harp_derived_variable_conversions->num_variables);
    if (info.skip == NULL)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not allocate % lu bytes) (%s:%u)",
                       (long)harp_derived_variable_conversions->num_variables, __FILE__, __LINE__);
        return -1;
    }
    memset(info.skip, 0, harp_derived_variable_conversions->num_variables);

    /* Show possible conversions */
    for (i = 0; i < harp_derived_variable_conversions->num_variables; i++)
    {
        harp_variable_conversion_list *conversion_list = harp_derived_variable_conversions->conversions_for_variable[i];
        harp_variable *variable;

        assert(conversion_list->num_conversions > 0);

        for (j = 0; j < conversion_list->num_conversions; j++)
        {
            harp_variable_conversion *conversion = conversion_list->conversion[j];
            int k;

            if (conversion->enabled != NULL && !conversion->enabled())
            {
                continue;
            }

            if (harp_product_get_variable_by_name(product, conversion_list->conversion[j]->variable_name, &variable) ==
                0)
            {
                if (harp_variable_has_dimension_types(variable, conversion_list->conversion[j]->num_dimensions,
                                                      conversion_list->conversion[j]->dimension_type))
                {
                    /* variable with same dimensions already exists -> skip conversions for this variable */
                    continue;
                }
            }

            info.skip[i] ^= 1 << conversion->num_dimensions;

            for (k = 0; k < conversion->num_source_variables; k++)
            {
                int result;

                result = find_source_variable(&info, &conversion->source_definition[k]);
                if (result == -1)
                {
                    free(info.skip);
                    return -1;
                }
                if (result == 0)
                {
                    /* source not found */
                    break;
                }
            }

            if (k == conversion->num_source_variables)
            {
                /* all sources are found, conversion should be possible */
                info.variable_name = conversion->variable_name;
                info.conversion = conversion;
                print_conversion_variable(conversion);
                print_conversion(&info, 1);
                printf("\n");
                /* don't show any remaining results */
                info.skip[i] ^= 1 << conversion->num_dimensions;
                break;
            }

            info.skip[i] ^= 1 << conversion->num_dimensions;
        }
    }

    free(info.skip);
    return 0;
}

/** Retrieve a new variable based on the set of automatic conversions that are supported by HARP.
 * \ingroup harp_product
 * If the product already contained a variable with the given name, you will get a copy of that variable (and converted
 * to the specified data type and unit). Otherwise the function will try to create a new variable based on the data
 * found in the product or on available auxiliary data (e.g. built-in climatology).
 * The caller of this function will be responsible for the memory management of the returned variable.
 * \note setting unit to NULL returns a variable in the original unit
 * \note pointers to axis variables are passed through unmodified.
 * \param product Product from which to derive the new variable.
 * \param name Name of the variable that should be created.
 * \param unit Unit (optional) of the variable that should be created.
 * \param num_dimensions Number of dimensions of the variable that should be created.
 * \param dimension_type Type of dimension for each of the dimensions of the variable that should be created.
 * \param variable Pointer to the C variable where the derived HARP variable will be stored.
 * \return
 *   \arg \c 0, Success.
 *   \arg \c -1, Error occurred (check #harp_errno).
 */
int harp_product_get_derived_variable(const harp_product *product, const char *name, const char *unit,
                                      int num_dimensions, const harp_dimension_type *dimension_type,
                                      harp_variable **variable)
{
    conversion_info info;
    int i;

    if (name == NULL)
    {
        harp_set_error(HARP_ERROR_INVALID_ARGUMENT, "name of variable to be derived is empty (%s:%u)", __FILE__,
                       __LINE__);
        return -1;
    }

    if (harp_product_get_variable_by_name(product, name, &info.variable) == 0)
    {
        if (harp_variable_has_dimension_types(info.variable, num_dimensions, dimension_type))
        {
            /* variable already exists -> create a copy */
            if (harp_variable_copy(info.variable, &info.variable) != 0)
            {
                return -1;
            }

            if (unit != NULL)
            {
                if (harp_variable_convert_unit(info.variable, unit) != 0)
                {
                    harp_variable_delete(info.variable);
                    return -1;
                }
            }
            *variable = info.variable;
            return 0;
        }
    }

    if (harp_derived_variable_conversions == NULL)
    {
        if (harp_derived_variable_list_init() != 0)
        {
            return -1;
        }
    }

    info.product = product;
    info.conversion = NULL;
    info.variable_name = name;
    info.num_dimensions = num_dimensions;
    for (i = 0; i < num_dimensions; i++)
    {
        info.dimension_type[i] = dimension_type[i];
    }
    info.skip = NULL;
    info.variable = NULL;

    info.skip = malloc(harp_derived_variable_conversions->num_variables);
    if (info.skip == NULL)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not allocate % lu bytes) (%s:%u)",
                       (long)harp_derived_variable_conversions->num_variables, __FILE__, __LINE__);
        return -1;
    }
    memset(info.skip, 0, harp_derived_variable_conversions->num_variables);

    if (find_and_execute_conversion(&info) != 0)
    {
        free(info.skip);
        return -1;
    }

    free(info.skip);

    if (unit != NULL)
    {
        if (harp_variable_convert_unit(info.variable, unit) != 0)
        {
            harp_variable_delete(info.variable);
            return -1;
        }
    }

    *variable = info.variable;
    return 0;
}

/** Create a derived variable and add it to the product.
 * \ingroup harp_product
 * If a similar named variable with the right dimensions was already in the product then that variable
 * will be modified to match the given unit
 * (and in case \a unit is NULL, then the function will just leave the product unmodified).
 * Otherwise the function will call harp_product_get_derived_variable() and add the new variable using
 * harp_product_add_variable() (removing any existing variable with the same name, but different dimensions)
 * \param product Product from which to derive the new variable and into which the derived variable should be placed.
 * \param name Name of the variable that should be added.
 * \param unit Unit (optional) of the variable that should be added.
 * \param num_dimensions Number of dimensions of the variable that should be created.
 * \param dimension_type Type of dimension for each of the dimensions of the variable that should be created.
 * \return
 *   \arg \c 0, Success.
 *   \arg \c -1, Error occurred (check #harp_errno).
 */
int harp_product_add_derived_variable(harp_product *product, const char *name, const char *unit,
                                      int num_dimensions, const harp_dimension_type *dimension_type)
{
    harp_variable *new_variable;
    harp_variable *variable = NULL;

    if (harp_product_get_variable_by_name(product, name, &variable) == 0)
    {
        if (harp_variable_has_dimension_types(variable, num_dimensions, dimension_type))
        {
            /* variable already exists */
            if (unit != NULL && !harp_variable_has_unit(variable, unit))
            {
                if (harp_variable_convert_unit(variable, unit) != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
    }

    if (harp_derived_variable_conversions == NULL)
    {
        if (harp_derived_variable_list_init() != 0)
        {
            return -1;
        }
    }

    /* variable with right dimensions does not yet exist -> create and add it */
    if (harp_product_get_derived_variable(product, name, unit, num_dimensions, dimension_type, &new_variable) != 0)
    {
        return -1;
    }
    if (variable != NULL)
    {
        /* remove existing variable with same name (but different dimension) */
        if (harp_product_remove_variable(product, variable) != 0)
        {
            harp_variable_delete(new_variable);
            return -1;
        }
    }
    if (harp_product_add_variable(product, new_variable) != 0)
    {
        harp_variable_delete(new_variable);
        return -1;
    }

    return 0;
}