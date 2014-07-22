/*
 * Copyright (c) 2014      Intel, Inc.  All rights reserved. 
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "orcm_config.h"
#include "opal/util/output.h"

#include "opal/mca/base/mca_base_var.h"

#include "analytics_average.h"

/*
 * Public string for version number
 */
const char *orcm_analytics_average_component_version_string =
    "ORCM ANALYTICS average MCA component version " ORCM_VERSION;

/*
 * Local functionality
 */
static int analytics_average_open(void);
static int analytics_average_close(void);
static int analytics_average_component_query(mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
orcm_analytics_base_component_t mca_analytics_average_component = {
    {
        ORCM_ANALYTICS_BASE_VERSION_1_0_0,
        /* Component name and version */
        "average",
        ORCM_MAJOR_VERSION,
        ORCM_MINOR_VERSION,
        ORCM_RELEASE_VERSION,
        
        /* Component open and close functions */
        analytics_average_open,
        analytics_average_close,
        analytics_average_component_query,
        NULL
    },
    {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int analytics_average_open(void)
{
    return ORCM_SUCCESS;
}

static int analytics_average_close(void)
{
    return ORCM_SUCCESS;
}

static int analytics_average_component_query(mca_base_module_t **module, int *priority)
{
    *priority = 5;
    *module = (mca_base_module_t *)&orcm_analytics_average_module;
    return ORCM_SUCCESS;
}
