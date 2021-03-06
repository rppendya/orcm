/*
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2013-2015 Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "orcm_config.h"
#include "orcm/constants.h"

#include <sys/stat.h>
#include <sqlite3.h>

#include "opal/mca/base/base.h"

#include "orte/mca/errmgr/errmgr.h"

#include "orcm/mca/db/db.h"
#include "orcm/mca/db/base/base.h"
#include "db_sqlite.h"

static int component_register(void);
static bool component_avail(void);
static orcm_db_base_module_t *component_create(opal_list_t *props);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
orcm_db_base_component_t mca_db_sqlite_component = {
    {
        ORCM_DB_BASE_VERSION_2_0_0,

        /* Component name and version */
        .mca_component_name = "sqlite",
        MCA_BASE_MAKE_VERSION(component, ORCM_MAJOR_VERSION, ORCM_MINOR_VERSION,
                              ORCM_RELEASE_VERSION),
        
        /* Component open and close functions */
        .mca_open_component = NULL,
        .mca_close_component = NULL,
        .mca_query_component = NULL,
        .mca_register_component_params = component_register
    },
    .base_data = {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
    .priority = 75,
    .available = component_avail,
    .create_handle = component_create,
    .finalize = NULL
};

static char *db_file;
static int num_worker_threads;
static int thread_safe;

static int component_register(void)
{
    mca_base_component_t *c = &mca_db_sqlite_component.base_version;

    /* retrieve the name of the file to be used */
    db_file = NULL;
    (void) mca_base_component_var_register (c, "database", "Name of file to be used for database",
                                            MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            OPAL_INFO_LVL_9,
                                            MCA_BASE_VAR_SCOPE_READONLY,
                                            &db_file);
    
    /* retrieve the number of worker threads to be used, if sqlite3 is thread-safe */
    num_worker_threads = -1;
    (void) mca_base_component_var_register (c, "num_worker_threads", "Number of worker threads to be used",
                                            MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            OPAL_INFO_LVL_9,
                                            MCA_BASE_VAR_SCOPE_READONLY,
                                            &num_worker_threads);

    return ORCM_SUCCESS;
}

static bool component_avail(void)
{
    /* initialize sqlite3 */
    if (SQLITE_OK != sqlite3_initialize()) {
        return false;
    }
    /* check if sqlite was built thread-safe - if not, we won't
     * use worker threads for thruput
     */
    thread_safe = sqlite3_threadsafe();

    return true;
}

static orcm_db_base_module_t *component_create(opal_list_t *props)
{
    mca_db_sqlite_module_t *mod;
    opal_value_t *kv;

    mod = (mca_db_sqlite_module_t*)malloc(sizeof(mca_db_sqlite_module_t));
    if (NULL == mod) {
        ORTE_ERROR_LOG(ORCM_ERR_OUT_OF_RESOURCE);
        return NULL;
    }
    memset(mod, 0, sizeof(mca_db_sqlite_module_t));
    mod->nthreads = -1;

    /* copy the APIs across */
    memcpy(mod, &mca_db_sqlite_module.api, sizeof(orcm_db_base_module_t));

    /* if the props include db info, then use it */
    OPAL_LIST_FOREACH(kv, props, opal_value_t) {
        if (0 == strcmp(kv->key, "database")) {
            mod->dbfile = strdup(kv->data.string);
        } else if (0 == strcmp(kv->key, "num_worker_threads")) {
            mod->nthreads = kv->data.integer;
        }
    }
    if (NULL == mod->dbfile) {
        if (NULL == db_file) {
            /* nothing was provided - opt out */
            free(mod);
            return NULL;
        }
        mod->dbfile = strdup(db_file);
    }
    if (0 != thread_safe) {
        mod->nthreads = 1;
    } else {
        if (0 > mod->nthreads && 0 < num_worker_threads) {
            mod->nthreads = num_worker_threads;
        } else {
            mod->nthreads = 1;
        }
    }

    /* let the module init */
    if (ORCM_SUCCESS != mod->api.init((struct orcm_db_base_module_t*)mod)) {
        mod->api.finalize((struct orcm_db_base_module_t*)mod);
        free(mod);
        return NULL;
    }

    return (orcm_db_base_module_t*)mod;
}
