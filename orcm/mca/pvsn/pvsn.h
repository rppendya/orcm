/*
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* This framework provides a mechanism callers can use
 * to request a provisioning action. Rather than specifying
 * a detailed image layout, we only allow the caller to
 * specify an existing image. Hence, we create a name for
 * each image (in Warewulf, this would be the equivalent of
 * the named chroot), and let the caller specify the
 * desired provision that way.
 *
 * However, that still doesn't cover all the attributes - e.g.,
 * the files to be included may vary on a node-by-node basis
 * and could be specified for a particular provisioning. In
 * this case, the base image would be the same, but the caller
 * just wants to modify some of the per-node attributes of the
 * image. So we also provide a mechanism for passing customized
 * attributes as part of the provisioning request. Since the
 * available attributes may be highly dependent on the backend
 * provisioning agent (and hence, the selection of active module),
 * we use the orte_attribute engine to define them in a generic
 * manner.
 *
 * Finally, given these choices, we have to provide a query
 * mechanism by which a caller can discover what images are
 * available, and what attributes are defined by default.
 *
 * All of this needs to be done asynchronously, so the framework
 * will have its own event base, and all calls into the active
 * module will execute within that base. This is done because the
 * actual provisioning agent may be slow, and we don't want to
 * block the caller until the operation is complete
 */
 
#ifndef MCA_PVSN_H
#define MCA_PVSN_H

/*
 * includes
 */

#include "orcm_config.h"
#include "orcm/constants.h"

#include "opal/mca/mca.h"
#include "opal/mca/event/event.h"
#include "opal/util/output.h"


BEGIN_C_DECLS

/* define an object to describe a provisionable resource. The
 * type is a name (e.g., "file"), and the attributes are a list
 * of opal_value_t objects that describe what is known
 * about that resource */
typedef struct {
    opal_list_item_t super;
    char *type;
    opal_list_t attributes;
} orcm_pvsn_resource_t;
OBJ_CLASS_DECLARATION(orcm_pvsn_resource_t);

/* define an image object to describe an image
 * we can provision */
typedef struct {
    opal_list_item_t super;
    char *name;
    opal_list_t attributes;
} orcm_pvsn_image_t;
OBJ_CLASS_DECLARATION(orcm_pvsn_image_t);

/* define an object that describes the provisioning
 * of a node - basically, this contains a regular
 * expression generated by the orte_regex functions
 * plus an orcm_pvsn_image_t object that describes
 * the specific provisioning for those nodes */
typedef struct {
    opal_list_item_t super;
    char *nodes;
    orcm_pvsn_image_t image;
} orcm_pvsn_provision_t;
OBJ_CLASS_DECLARATION(orcm_pvsn_provision_t);

/* define the callback function for provisioning requests
 * as we only have to return the status indicating whether
 * or not the request succeeded, plus any user-provided data */
typedef void (*orcm_pvsn_provision_cbfunc_t)(int status, void *cbdata);

/* define the callback function for resource queries that
 * return a status indicating any error. In the case of
 * a query for available resources, the list will contain
 * orcm_pvsn_resource_t objects that describe the currently-available
 * resources. In the case of a query for current provisioning,
 * the list will contain orcm_pvsn_provision_t objects.
 * In both cases, the PVSN calling function will OWN the
 * list members - thus, if the callback function wishes
 * to retain any of them, it needs to either safely remove them
 * from the list or OBJ_RETAIN the desired item (note: the
 * item can only exist on one list at a time, so moving the
 * item to another list requires first removing it from the
 * provided list) */
typedef void (*orcm_pvsn_query_cbfunc_t)(int status,
                                         opal_list_t *images,
                                         void *cbdata);

/*
 * Component functions - all MUST be provided!
 */

/* initialize the selected module */
typedef int (*orcm_pvsn_base_module_init_fn_t)(void);

/* finalize the selected module */
typedef void (*orcm_pvsn_base_module_finalize_fn_t)(void);

/* query the provisioning agent for a list of available resources. The
 * type of resources of interest can be specified as a comma-separated
 * list. The returned list will consist of orcm_pvsn_resource_t objects */
typedef int (*orcm_pvsn_base_module_avail_fn_t)(char *resource, opal_list_t *available);

/* query the provisioning status of a set of nodes. This will return a
 * list of orcm_pvsn_provision_t objects. One object will  be returned for each unique
 * combination of image+attributes. The  node names for which info is
 * being requested can be NULL (to request info  for all nodes), or a
 * regular expression parseable by the orte_regex fns */
typedef int (*orcm_pvsn_base_module_status_fn_t)(char *nodes, opal_list_t *images);

/* provision one or more nodes with a specific image and attributes */
typedef int (*orcm_pvsn_base_module_provision_fn_t)(char *nodes,
                                                    char *image,
                                                    opal_list_t *attributes);

/*
 * Ver 1.0
 */
typedef struct {
    orcm_pvsn_base_module_init_fn_t        init;
    orcm_pvsn_base_module_finalize_fn_t    finalize;
    orcm_pvsn_base_module_avail_fn_t       get_available;
    orcm_pvsn_base_module_status_fn_t      get_status;
    orcm_pvsn_base_module_provision_fn_t   provision;
} orcm_pvsn_base_module_t;

/* define a public API for requesting a provisioning action */
typedef void (*orcm_pvsn_base_API_provision_fn_t)(char *nodes,
                                                  char *image,
                                                  opal_list_t *attributes,
                                                  orcm_pvsn_provision_cbfunc_t cbfunc,
                                                  void *cbdata);

/* define a public API for querying the current provisioning status of
 * one or more nodes. This will return a list of orcm_pvsn_image_t objects
 * that contain a regular expression for the node name(s), the name
 * of the base image on them, and any customized attributes. One object will
 * be returned for each unique combination of image+attributes. The
 * node names for which info is being requested can be NULL (to request info
 * for all nodes), or a regular expression parseable by the orte_regex fns */
typedef void (*orcm_pvsn_base_API_query_status_fn_t)(char *nodes,
                                                     orcm_pvsn_query_cbfunc_t cbfunc,
                                                     void *cbdata);

/* define a public API for querying info on available provisioning resources. The
 * type of resources of interest can be specified as a comma-separated
 * list. The returned list will consist of orcm_pvsn_resource_t objects */
typedef void (*orcm_pvsn_base_API_query_avail_fn_t)(char *resource,
                                                    orcm_pvsn_query_cbfunc_t cbfunc,
                                                    void *cbdata);

typedef struct {
    orcm_pvsn_base_API_query_avail_fn_t    get_available_images;
    orcm_pvsn_base_API_query_status_fn_t   get_current_status;
    orcm_pvsn_base_API_provision_fn_t      provision;
} orcm_pvsn_API_module_t;

/*
 * the standard component data structure
 */
typedef struct {
    mca_base_component_t base_version;
    mca_base_component_data_t base_data;
} orcm_pvsn_base_component_t;

/*
 * Macro for use in components that are of type pvsn v1.0.0
 */
#define ORCM_PVSN_BASE_VERSION_1_0_0 \
  /* pvsn v1.0 is chained to MCA v2.0 */ \
  MCA_BASE_VERSION_2_0_0, \
  /* pvsn v1.0 */ \
  "pvsn", 1, 0, 0

/* Global structure for accessing name server functions
 */
ORCM_DECLSPEC extern orcm_pvsn_API_module_t orcm_pvsn;  /* holds public API function pointers */

END_C_DECLS

#endif
