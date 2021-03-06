/*
 * Copyright (c) 2013-2015 Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "opal/dss/dss.h"

#include "orcm_config.h"
#include "orcm/constants.h"
#include "orcm/types.h"

#include <string.h>
#include <pthread.h>

#define HAVE_HWLOC_DIFF  // protect the hwloc diff.h file from ipmicmd.h conflict
#include "orcm/mca/sensor/base/base.h"
#include "orcm/mca/sensor/base/sensor_private.h"
#include "orcm/mca/db/db.h"
#include "orcm/runtime/orcm_globals.h"

#include "orte/util/show_help.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "opal/runtime/opal_progress_threads.h"

#include "sensor_ipmi.h"
#include <../share/ipmiutil/isensor.h>

typedef struct {
    opal_event_base_t *ev_base;
    bool ev_active;
    int sample_rate;
} orcm_sensor_ipmi_t;

/* declare the API functions */
static int init(void);
static void finalize(void);
static void start(orte_jobid_t job);
static void stop(orte_jobid_t job);
static void ipmi_sample(orcm_sensor_sampler_t *sampler);
static void perthread_ipmi_sample(int fd, short args, void *cbdata);
static void collect_sample(orcm_sensor_sampler_t *sampler);
static void ipmi_log(opal_buffer_t *buf);
static void ipmi_inventory_collect(opal_buffer_t *inventory_snapshot);
static void ipmi_inventory_log(char *hostname, opal_buffer_t *inventory_snapshot);
static void ipmi_set_sample_rate(int sample_rate);
static void ipmi_get_sample_rate(int *sample_rate);
int count_log = 0;

char **sensor_list_token; /* 2D array storing multiple sensor keywords for collecting metrics */

opal_list_t sensor_active_hosts; /* Hosts list for collecting metrics */
opal_list_t ipmi_inventory_hosts; /* Hosts list for storing inventory details */
static orcm_sensor_sampler_t *ipmi_sampler = NULL;
static orcm_sensor_ipmi_t orcm_sensor_ipmi;

static void ipmi_con(orcm_sensor_hosts_t *host)
{
}
static void ipmi_des(orcm_sensor_hosts_t *host)
{
}
OBJ_CLASS_INSTANCE(orcm_sensor_hosts_t,
                   opal_list_item_t,
                   ipmi_con, ipmi_des);

orcm_sensor_hosts_t *current_host_configuration;

typedef struct {
    opal_list_item_t super;
    char *nodename;
    unsigned long hashId; /* A hash value summing up the inventory record for each node, for quick comparision */
    opal_list_t *records; /* An hwloc topology container followed by a list of inventory items */
} ipmi_inventory_t;

static void inv_con(ipmi_inventory_t *trk)
{
    trk->records = OBJ_NEW(opal_list_t);
}
static void inv_des(ipmi_inventory_t *trk)
{
    if(NULL != trk) {
        if(NULL != trk->records) {
            OPAL_LIST_RELEASE(trk->records);
        }
        if(NULL != trk->nodename) {
            free(trk->nodename);
        }
    }
}
OBJ_CLASS_INSTANCE(ipmi_inventory_t,
                   opal_list_item_t,
                   inv_con, inv_des);


/* instantiate the module */
orcm_sensor_base_module_t orcm_sensor_ipmi_module = {
    init,
    finalize,
    start,
    stop,
    ipmi_sample,
    ipmi_log,
    ipmi_inventory_collect,
    ipmi_inventory_log,
    ipmi_set_sample_rate,
    ipmi_get_sample_rate
};

/* local variables */
static opal_buffer_t test_vector;
static void generate_test_vector(opal_buffer_t *v);
static void generate_test_vector_inv(opal_buffer_t *inventory_snapshot);
static time_t last_sample = 0;
static bool log_enabled = true;


char ipmi_inv_tv[5][2][30] = {
{"bmc_ver","9.9"},
{"ipmi_ver","8.8"},
{"bb_serial","TV_BbSer"},
{"bb_vendor","TV_BbVen"},
{"bb_manufactured_date","TV_MaufDat"}};

static int init(void)
{
    int rc;
    disable_ipmi = 0;

    OBJ_CONSTRUCT(&sensor_active_hosts, opal_list_t);
    OBJ_CONSTRUCT(&ipmi_inventory_hosts, opal_list_t);
    current_host_configuration = OBJ_NEW(orcm_sensor_hosts_t);
    if (mca_sensor_ipmi_component.test) {
        /* generate test vector */
        OBJ_CONSTRUCT(&test_vector, opal_buffer_t);
        generate_test_vector(&test_vector);
        return OPAL_SUCCESS;
    }

    rc = orcm_sensor_ipmi_get_bmc_cred(current_host_configuration);
    if(rc != ORCM_SUCCESS) {
        opal_output(0, "Unable to collect the current host details");
        return ORCM_ERROR;
    }

    return OPAL_SUCCESS;
}

static void finalize(void)
{
    OPAL_LIST_DESTRUCT(&sensor_active_hosts);
    OPAL_LIST_DESTRUCT(&ipmi_inventory_hosts);
    OBJ_DESTRUCT(current_host_configuration);
}

/*Start monitoring of local processes */
static void start(orte_jobid_t jobid)
{
    /* Select sensor list if no sensors are specified by the user */
    if((NULL==mca_sensor_ipmi_component.sensor_list) & (NULL==mca_sensor_ipmi_component.sensor_group))
    {
        sensor_list_token = opal_argv_split("PS1 Power In,PS1 Temperature",',');
    } else {
        sensor_list_token = opal_argv_split(mca_sensor_ipmi_component.sensor_list,',');
    }
    for(int i =0; i <opal_argv_count(sensor_list_token);i++)
    {
        opal_output(0,"Sensor %d: %s",i,sensor_list_token[i]);
    }
    if(NULL!=mca_sensor_ipmi_component.sensor_group)
    {
        opal_output(0, "sensor group selected: %s", mca_sensor_ipmi_component.sensor_group);
    }

    /* start a separate ipmi progress thread for sampling */
    if (mca_sensor_ipmi_component.use_progress_thread) {
        if (!orcm_sensor_ipmi.ev_active) {
            orcm_sensor_ipmi.ev_active = true;
            if (NULL == (orcm_sensor_ipmi.ev_base = opal_start_progress_thread("ipmi", true))) {
                orcm_sensor_ipmi.ev_active = false;
                return;
            }
        }

        /* setup ipmi sampler */
        ipmi_sampler = OBJ_NEW(orcm_sensor_sampler_t);

        /* check if ipmi sample rate is provided for this*/
        if (mca_sensor_ipmi_component.sample_rate) {
            ipmi_sampler->rate.tv_sec = mca_sensor_ipmi_component.sample_rate;
        } else {
            ipmi_sampler->rate.tv_sec = orcm_sensor_base.sample_rate;
        } 

        ipmi_sampler->log_data = orcm_sensor_base.log_samples;
        opal_event_evtimer_set(orcm_sensor_ipmi.ev_base, &ipmi_sampler->ev,
                               perthread_ipmi_sample, ipmi_sampler);
        opal_event_evtimer_add(&ipmi_sampler->ev, &ipmi_sampler->rate);
    }
    return;
}

static void stop(orte_jobid_t jobid)
{
    count_log = 0;
    if (orcm_sensor_ipmi.ev_active) {
        orcm_sensor_ipmi.ev_active = false;
        /* stop the thread without releasing the event base */
        opal_stop_progress_thread("ipmi", false);
    }
    return;
}

int orcm_sensor_ipmi_get_bmc_cred(orcm_sensor_hosts_t *host)
{
    unsigned char idata[4], idata1[4], rdata[20];
	unsigned char ccode;
    char bmc_ip[16];
    int rlen = 20;
    int ret = 0;
    char *error_string;
    device_id_t devid;
    char test[16], test1[16];

    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
        "RETRIEVING LAN CREDENTIALS");
    strncpy(host->capsule.node.host_ip,"CUR_HOST_IP",strlen("CUR_HOST_IP")+1);

    /* This IPMI call reading the BMC's IP address runs through the
     *  ipmi/imb/KCS driver
     */
    memset(idata,0x00,4);

    /* Read IP Address - Ref Table 23-* LAN Config Parameters of 
     * IPMI v2 Rev 1.1
     */
    idata[1] = GET_BMC_IP_CMD; 
    for(idata[0] = 0; idata[0]<16;idata[0]++)
    {
        ret = ipmi_cmd(GET_LAN_CONFIG, idata, 4, rdata, &rlen,&ccode, 0);
        if(ret)
        {
            error_string = decode_rv(ret);
            //orte_show_help("help-orcm-sensor-ipmi.txt", "ipmi-cmd-fail",
            //               true, orte_process_info.nodename, error_string);
            if (ERR_NO_DRV == ret) {
                return ORCM_ERROR;
            } else {
                rlen=20;
                continue;
            }
        }
        ipmi_close();
        if(ccode == 0)
        {
            sprintf(bmc_ip,"%d.%d.%d.%d",rdata[1], rdata[2], rdata[3], rdata[4]);
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                        "RETRIEVED BMC's IP ADDRESS: %s",bmc_ip);
            strncpy(host->capsule.node.bmc_ip,bmc_ip,strlen(bmc_ip)+1);
            strncpy(host->capsule.node.user,"CUR_USERNAME",strlen("CUR_USERNAME")+1);
            strncpy(host->capsule.node.pasw,"CUR_PASSWORD",strlen("CUR_PASSWORD")+1);
            orcm_sensor_get_fru_inv(host);

            /* Get the DEVICE ID information as well */
            ret = ipmi_cmd(GET_DEVICE_ID, idata1, 0, rdata, &rlen, &ccode, 0);
            if(0 == ret)
            {
                ipmi_close();
                memcpy(&devid.raw, rdata, sizeof(devid));

                /*  Pack the BMC FW Rev */
                sprintf(test,"%x", devid.bits.fw_rev_1&0x7F);
                sprintf(test1,"%x", devid.bits.fw_rev_2&0xFF);
                strcat(test,".");
                strcat(test,test1);
                strncpy(host->capsule.prop.bmc_rev, test, sizeof(test));

                /*  Pack the IPMI VER */
                sprintf(test,"%x", devid.bits.ipmi_ver&0xF);
                sprintf(test1,"%x", devid.bits.ipmi_ver&0xF0);
                strcat(test,".");
                strcat(test,test1);
                strncpy(host->capsule.prop.ipmi_ver, test, sizeof(test));
            }

            else {
                error_string = decode_rv(ret);
                opal_output(0,"Unable to collect IPMI Device ID information: %s",error_string);
            }

            return ORCM_SUCCESS;
        } else {
            opal_output_verbose(2, orcm_sensor_base_framework.framework_output,
                        "Received a non-zero ccode: %d, relen:%d", ccode, rlen);
        }
        rlen=20;
    }

    return ORCM_ERROR;
}

int orcm_sensor_get_fru_inv(orcm_sensor_hosts_t *host)
{
    int ret = 0;
    unsigned char idata[4], rdata[MAX_FRU_DEVICES][256];
    unsigned char ccode;
    int rlen = 256;
    int id;
    int max_id = 0;;
    unsigned char hex_val;
    long int fru_area;
    long int max_fru_area = 0;
    char *error_string;

    memset(idata,0x00,4);
    
    hex_val = 0x00;

    for (id = 0; id < MAX_FRU_DEVICES; id++) {
        memset(rdata[id], 0x00, 256);
        *idata = hex_val;
        ret = ipmi_cmd(GET_FRU_INV_AREA, idata, 1, rdata[id], &rlen, &ccode, 0);
        if(ret)
        {
            error_string = decode_rv(ret);
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "ipmi_cmd_mc for get_fru_inv RETURN CODE : %s \n", error_string);
        }
        ipmi_close();
        hex_val++;
    }
    /*
    Now that we have the size of each fru device, we want to find the one
    with the largest size, as that is the one we'll want to read from.
    */
    for (id = 0; id < MAX_FRU_DEVICES; id++) {
        /* Convert the hex value in rdata to decimal so we can compare it.*/
        fru_area = rdata[id][0] | (rdata[id][1] << 8) | (rdata[id][2] << 16) | (rdata[id][3] << 24);

        /* 
        If the newest area is the larget, set the max size to that and
        mark the max id to be that id.
        */
        if (fru_area > max_fru_area) {
            max_fru_area = fru_area;
            max_id = id;
        }
    }
    return orcm_sensor_get_fru_data(max_id, max_fru_area, host);
}

int orcm_sensor_get_fru_data(int id, long int fru_area, orcm_sensor_hosts_t *host)
{
    int ret;
    int i, ffail_count=0;
    int rlen = 256;
    unsigned char idata[4];
    unsigned char tempdata[17];
    unsigned char *rdata;
    unsigned char ccode;
    int rdata_offset = 0;
    unsigned char fru_offset;

    unsigned int manuf_time[3]; /*holds the manufactured time (in minutes) from 0:00 1/1/96*/
    unsigned long int manuf_minutes; /*holds the manufactured time (in minutes) from 0:00 1/1/96*/
    unsigned long int manuf_seconds; /*holds the above time (in seconds)*/
    time_t raw_seconds;
    struct tm *time_info;
    char manuf_date[11]; /*A mm/dd/yyyy or dd/mm/yyyy formatted date 10 + 1 for null byte*/

    unsigned char board_manuf_length; /*holds the length (in bytes) of board manuf name*/
    char *board_manuf; /*hold board manufacturer*/
    unsigned char board_product_length; /*holds the length (in bytes) of board product name*/
    char *board_product_name; /*holds board product name*/
    unsigned char board_serial_length; /*holds length (in bytes) of board serial number*/
    char *board_serial_num; /*will hold board serial number*/
    unsigned char board_part_length; /*holds length (in bytes) of the board part number*/
    char *board_part_num; /*will hold board part number*/

    rdata = (unsigned char*) malloc(fru_area);

    if (NULL == rdata) {
        return ORCM_ERROR;
    }

    memset(rdata,0x00,sizeof(rdata));
    memset(idata,0x00,sizeof(idata));

    idata[0] = id;   /*id of the fru device to read from*/
    idata[1] = 0x00; /*LSByte of the offset, start at 0*/
    idata[2] = 0x00; /*MSbyte of the offset, start at 0*/
    idata[3] = 0x10; /*reading 16 bytes at a time*/
    ret = 0;
    for (i = 0; i < (fru_area/16); i++) {
        memset(tempdata, 0x00, sizeof(tempdata));
        ret = ipmi_cmd(READ_FRU_DATA, idata, 4, tempdata, &rlen, &ccode, 0);
        if (ret) {
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                                "FRU Read Number %d retrying in block %d\n", id, i);
            ipmi_close();
            if (ffail_count > 15)
            {
                orte_show_help("help-orcm-sensor-ipmi.txt", "ipmi-fru-read-fail",
                               true, orte_process_info.nodename);
                free(rdata);
                return ORCM_ERROR;
            } else {
                i--;
                ffail_count++;
                continue;
            }
        }
        ipmi_close();

        /*
        Copy what was read in to the next 16 byte section of rdata
        and then increment the offset by another 16 for the next read
        */
        memcpy(rdata + rdata_offset, &tempdata[1], 16);
        rdata_offset += 16;

        /*We need to increment the MSByte instead of the LSByte*/
        if (idata[1] == 240) {
            idata[1] = 0x00;
            idata[2]++;
        }

        else {
            idata[1] += 0x10;
        }
    }

    /*
        Source: Platform Management Fru Document (Rev 1.2)
        Fru data is stored in the following fashion:
        1 Byte - N number of bytes to follow that holds some information
        N Bytes - The information we are after

        So the location of information within rdata is always relative to the
        location of the information that came before it.

        To get to the size of the information to follow, skip past all the
        information you've already read. To the read that information, skip
        past all the information you've already read + 1, then read that number
        of bytes.
    */

    /* Board Info */
    fru_offset = rdata[3] * 8; /*Board starting offset is stored in 3, multiples of 8 bytes*/

    /*IPMI time is stored in minutes from 0:00 1/1/1996*/
    manuf_time[0] = rdata[fru_offset + 3]; /*LSByte of the time*/
    manuf_time[1] = rdata[fru_offset + 4]; /*MiddleByte of the time*/
    manuf_time[2] = rdata[fru_offset + 5]; /*MSByte of the time*/

    /*Convert to 1 value*/
    manuf_minutes = manuf_time[0] + (manuf_time[1] << 8) + (manuf_time[2] << 16);
    manuf_seconds = manuf_minutes * 60;

    /*Time from epoch = time from ipmi start + difference from epoch to ipmi start*/
    raw_seconds = manuf_seconds + EPOCH_IPMI_DIFF_TIME;
    time_info = localtime(&raw_seconds);
    if (NULL == time_info) {
        free(rdata);
        return ORCM_ERROR;
    }
    else {
        strftime(manuf_date,10,"%x",time_info);
    }

    strncpy(host->capsule.prop.baseboard_manuf_date, manuf_date, sizeof(host->capsule.prop.baseboard_manuf_date)-1);
    host->capsule.prop.baseboard_manuf_date[sizeof(host->capsule.prop.baseboard_manuf_date)-1] = '\0';

    /*
        The 2 most significant bytes correspont to the length "type code".
        We assume, via the 0x3f mask, that the data is in English ASCII.
    */

    board_manuf_length = rdata[fru_offset + BOARD_INFO_DATA_START] & 0x3f;
    board_manuf = (char*) malloc (board_manuf_length + 1); /* + 1 for the Null Character */
    
    if (NULL == board_manuf) {
        free(rdata);
        return ORCM_ERROR;
    }

    for(i = 0; i < board_manuf_length; i++){
        board_manuf[i] = rdata[fru_offset + BOARD_INFO_DATA_START + 1 + i];
    }

    board_manuf[i] = '\0';
    strncpy(host->capsule.prop.baseboard_manufacturer, board_manuf, sizeof(host->capsule.prop.baseboard_manufacturer)-1);
    host->capsule.prop.baseboard_manufacturer[sizeof(host->capsule.prop.baseboard_manufacturer)-1] = '\0';

    board_product_length = rdata[fru_offset + BOARD_INFO_DATA_START + 1 + board_manuf_length] & 0x3f;
    board_product_name = (char*) malloc (board_product_length + 1); /* + 1 for the Null Character */

    if (NULL == board_product_name) {
        free(rdata);
        free(board_manuf);
        return ORCM_ERROR;
    }

    for(i = 0; i < board_product_length; i++) {
        board_product_name[i] = rdata[fru_offset + BOARD_INFO_DATA_START + 1 + board_manuf_length + 1 + i];
    }

    board_product_name[i] = '\0';
    strncpy(host->capsule.prop.baseboard_name, board_product_name, sizeof(host->capsule.prop.baseboard_serial)-1);
    host->capsule.prop.baseboard_name[sizeof(host->capsule.prop.baseboard_name)-1] = '\0';

    board_serial_length = rdata[fru_offset + BOARD_INFO_DATA_START + 1 + board_manuf_length + 1 + board_product_length] & 0x3f;
    board_serial_num = (char*) malloc (board_serial_length + 1); /* + 1 for the Null Character */

    if (NULL == board_serial_num) {
        free(rdata);
        free(board_manuf);
        free(board_product_name);
        return ORCM_ERROR;
    }

    for(i = 0; i < board_serial_length; i++) {
        board_serial_num[i] = rdata[fru_offset + BOARD_INFO_DATA_START + 1 + board_manuf_length + 1 + board_product_length + 1 + i];
    }

    board_serial_num[i] = '\0';
    strncpy(host->capsule.prop.baseboard_serial, board_serial_num, sizeof(host->capsule.prop.baseboard_serial)-1);
    host->capsule.prop.baseboard_serial[sizeof(host->capsule.prop.baseboard_serial)-1] = '\0';

    board_part_length = rdata[fru_offset + BOARD_INFO_DATA_START + 1 + board_manuf_length + 1 + board_product_length + board_serial_length + 1] & 0x3f;
    board_part_num = (char*) malloc (board_part_length + 1); /* + 1 for the Null Character */

    if (NULL == board_part_num) {
        free(rdata);
        free(board_manuf);
        free(board_product_name);
        free(board_serial_num);
        return ORCM_ERROR;
    }

    for (i = 0; i < board_part_length; i++) {
        board_part_num[i] = rdata[fru_offset + BOARD_INFO_DATA_START + 1 + board_manuf_length + 1 + board_product_length + 1 + board_serial_length + 1 + i];
    }

    board_part_num[i] = '\0';
    strncpy(host->capsule.prop.baseboard_part, board_part_num, sizeof(host->capsule.prop.baseboard_part)-1);
    host->capsule.prop.baseboard_part[sizeof(host->capsule.prop.baseboard_part)-1] = '\0';

    free(rdata);
    free(board_manuf);
    free(board_product_name);
    free(board_serial_num);
    free(board_part_num);
    return ORCM_SUCCESS;
}

/* int orcm_sensor_ipmi_found (char* nodename, opal_list_t * host_list)
 * Return ORCM_SUCCESS if nodename matches an existing node in the host_list
 * Return ORCM_ERR_NOT_FOUND if nodename doesn't match
 */
int orcm_sensor_ipmi_found(char *nodename, opal_list_t *host_list)
{
    orcm_sensor_hosts_t *host, *nxt;
    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
        "Finding Node: %s", nodename);
    OPAL_LIST_FOREACH_SAFE(host, nxt, host_list, orcm_sensor_hosts_t) {
        if(!strcmp(nodename,host->capsule.node.name))
        {
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                                "Found Node: %s", nodename);
            return ORCM_SUCCESS;
        }
    }
    return ORCM_ERR_NOT_FOUND;
}

int orcm_sensor_ipmi_addhost(char *nodename, char *host_ip, char *bmc_ip, opal_list_t *host_list)
{
    orcm_sensor_hosts_t *newhost;
    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                        "Adding New Node: %s, with BMC IP: %s", nodename, bmc_ip);

    newhost = OBJ_NEW(orcm_sensor_hosts_t);
    strncpy(newhost->capsule.node.name,nodename,sizeof(newhost->capsule.node.name)-1);
    strncpy(newhost->capsule.node.host_ip,host_ip,sizeof(newhost->capsule.node.host_ip)-1);
    strncpy(newhost->capsule.node.bmc_ip,bmc_ip,sizeof(newhost->capsule.node.bmc_ip)-1);

    /* Add to Host list */
    opal_list_append(host_list, &newhost->super);

    return ORCM_SUCCESS;
}

int orcm_sensor_ipmi_label_found(char *sensor_label)
{
    int i;
    for (i = 0; i< opal_argv_count(sensor_list_token);i++)
    {
        if(!strncmp(sensor_list_token[i],sensor_label,strlen(sensor_list_token[i])))
        {
            return 1;
        }
    }
    return 0;
}

static ipmi_inventory_t* found_inventory_host(char * nodename)
{
    ipmi_inventory_t *host;
    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
        "Finding Node in inventory inventory: %s", nodename);
    OPAL_LIST_FOREACH(host, &ipmi_inventory_hosts, ipmi_inventory_t) {
        if(!strcmp(nodename,host->nodename))
        {
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                                "Found Node: %s", nodename);
            return host;
        }
    }
    return NULL;
}

static bool compare_ipmi_record (ipmi_inventory_t* newhost , ipmi_inventory_t* oldhost)
{
    orcm_metric_value_t *newitem, *olditem;
    unsigned int count = 0, record_size = 0;
    /* @VINFIX: Need to come up with a clever way to implement comparision of different 
     * ipmi inventory records
     */
    if((record_size = opal_list_get_size(newhost->records)) != opal_list_get_size(oldhost->records))
    {
        opal_output(0,"IPMI Inventory compare failed: Unequal item count;");
        return false;
    }
    newitem = (orcm_metric_value_t*)opal_list_get_first(newhost->records);
    olditem = (orcm_metric_value_t*)opal_list_get_first(oldhost->records);

    for(count = 0; count < record_size ; count++) {
        if(newitem->value.type != olditem->value.type) {
            opal_output(0,"IPMI inventory records mismatch: value.type mismatch");
            return false;
        } else {
            if(OPAL_STRING == newitem->value.type) {
                if(strcmp(newitem->value.key, olditem->value.key)) {
                    opal_output(0,"IPMI inventory records mismatch: value.key mismatch");
                    return false;
                } else if (strcmp(newitem->value.data.string, olditem->value.data.string)) {
                    opal_output(0,"IPMI inventory records mismatch: value.data.string mismatch");
                    return false;
                }
            } else if ((OPAL_FLOAT == newitem->value.type)){
                if(newitem->value.data.fval != olditem->value.data.fval) {
                    opal_output(0,"IPMI inventory records mismatch: value.data.fval mismatch");
                    return false;
                }
            } else {
                opal_output(0,"Invalid data stored as inventory with invalid data type %d", newitem->value.type);
                return false;
            }
        }
        newitem = (orcm_metric_value_t*)opal_list_get_next(newitem);
        olditem = (orcm_metric_value_t*)opal_list_get_next(olditem);
    }

    return true;
}
static void ipmi_inventory_log(char *hostname, opal_buffer_t *inventory_snapshot)
{
    char *inv, *inv_val;
    unsigned int tot_items;
    int rc, n;
    ipmi_inventory_t *newhost, *oldhost;
    orcm_metric_value_t *mkv, *mkv_copy;

    newhost = OBJ_NEW(ipmi_inventory_t);
    newhost->nodename = strdup(hostname);

    n=1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(inventory_snapshot, &tot_items, &n, OPAL_UINT))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    while(tot_items > 0)
    {
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(inventory_snapshot, &inv, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(inventory_snapshot, &inv_val, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        /* opal_output(0,"%s: %s",inv, inv_val);*/
        
        mkv = OBJ_NEW(orcm_metric_value_t);
        mkv->value.key = inv;

        if(!strncmp(inv,"bmc_ver",sizeof("bmc_ver")) | !strncmp(inv,"ipmi_ver",sizeof("ipmi_ver")))
        {
            mkv->value.type = OPAL_FLOAT;
            mkv->value.data.fval = strtof(inv_val,NULL);
        } else {
            mkv->value.type = OPAL_STRING;
            mkv->value.data.string = inv_val;
        }
        opal_list_append(newhost->records, (opal_list_item_t *)mkv);
        tot_items--;
    }

    if(NULL != (oldhost = found_inventory_host(hostname)))
    {
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                            "Host '%s' Found, comparing values",hostname);
        if(false == compare_ipmi_record(newhost, oldhost))
        {
            opal_output(0,"IPMI Compare failed; Notify User; Update List; Update Database");
            OPAL_LIST_RELEASE(oldhost->records);
            oldhost->records=OBJ_NEW(opal_list_t);
            OPAL_LIST_FOREACH(mkv, newhost->records, orcm_metric_value_t) {
                mkv_copy = OBJ_NEW(orcm_metric_value_t);
                mkv_copy->value.key = strdup(mkv->value.key);

                if(!strncmp(mkv->value.key,"bmc_ver",sizeof("bmc_ver")) | !strncmp(mkv->value.key,"ipmi_ver",sizeof("ipmi_ver")))
                {
                    mkv_copy->value.type = OPAL_FLOAT;
                    mkv_copy->value.data.fval = mkv->value.data.fval;
                } else {
                    mkv_copy->value.type = OPAL_STRING;
                    mkv_copy->value.data.string = strdup(mkv->value.data.string);
                }
                opal_list_append(oldhost->records,(opal_list_item_t *)mkv_copy);   
            }
            /* Send the collected inventory details to the database for storage */
            if (0 <= orcm_sensor_base.dbhandle) {
                orcm_db.update_node_features(orcm_sensor_base.dbhandle, oldhost->nodename , oldhost->records, NULL, NULL);
            }
        } else {
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                                "ipmi compare passed");
        }
        /* newhost structure can be destroyed after comparision with original list and update */
        OBJ_DESTRUCT(newhost);

    } else {
        /* Append the new node to the existing host list */
        opal_list_append(&ipmi_inventory_hosts, &newhost->super);
        
        /* Send the collected inventory details to the database for storage */
        if (0 <= orcm_sensor_base.dbhandle) {
            orcm_db.update_node_features(orcm_sensor_base.dbhandle, newhost->nodename , newhost->records, NULL, NULL);
        }
    }
}

static void ipmi_set_sample_rate(int sample_rate)
{
    /* set the ipmi sample rate if seperate thread is enabled */
    if (mca_sensor_ipmi_component.use_progress_thread) {
        mca_sensor_ipmi_component.sample_rate = sample_rate;
    }
    return;
}

static void ipmi_get_sample_rate(int *sample_rate)
{
    if (NULL != sample_rate) {
    /* check if ipmi sample rate is provided for this*/
        if (mca_sensor_ipmi_component.use_progress_thread) {
            *sample_rate = mca_sensor_ipmi_component.sample_rate;
        }
    }
    return;
}

static void ipmi_inventory_collect(opal_buffer_t *inventory_snapshot)
{
    orcm_sensor_hosts_t *cur_host;
    int rc;
    unsigned int tot_items = 5;
    char *comp = strdup("ipmi");
    cur_host = current_host_configuration;
    
    if (mca_sensor_ipmi_component.test) {
        /* generate test vector */
        generate_test_vector_inv(inventory_snapshot);
        return;
    }

    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    free(comp);
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &tot_items, 1, OPAL_UINT))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    comp = "bmc_ver";
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    comp = cur_host->capsule.prop.bmc_rev;
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    comp = "ipmi_ver";
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    comp = cur_host->capsule.prop.ipmi_ver;
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    comp = "bb_serial";
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    comp = cur_host->capsule.prop.baseboard_serial;
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    comp = "bb_vendor";
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    comp = cur_host->capsule.prop.baseboard_manufacturer;
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    comp = "bb_manufactured_date";
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    comp = cur_host->capsule.prop.baseboard_manuf_date;
    if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
}

static void ipmi_sample(orcm_sensor_sampler_t *sampler)
{
    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                            "%s sensor ipmi : ipmi_sample: called",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    if (!mca_sensor_ipmi_component.use_progress_thread) {
       collect_sample(sampler);
    }

}

static void perthread_ipmi_sample(int fd, short args, void *cbdata)
{
    orcm_sensor_sampler_t *sampler = (orcm_sensor_sampler_t*)cbdata;

    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                            "%s sensor ipmi : perthread_ipmi_sample: called",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    
    /* this has fired in the sampler thread, so we are okay to
     * just go ahead and sample since we do NOT allow both the
     * base thread and the component thread to both be actively
     * calling this component */
    collect_sample(sampler);
    /* we now need to push the results into the base event thread
     * so it can add the data to the base bucket */
    ORCM_SENSOR_XFER(&sampler->bucket);
    /* clear the bucket */
    OBJ_DESTRUCT(&sampler->bucket);
    OBJ_CONSTRUCT(&sampler->bucket, opal_buffer_t);
    /* check if ipmi sample rate is provided for this*/
    if (mca_sensor_ipmi_component.sample_rate != sampler->rate.tv_sec) {
        sampler->rate.tv_sec = mca_sensor_ipmi_component.sample_rate;
    } 
    /* set ourselves to sample again */
    opal_event_evtimer_add(&sampler->ev, &sampler->rate);
}

static void collect_sample(orcm_sensor_sampler_t *sampler)
{
    int rc;
    opal_buffer_t data, *bptr;
    char *ipmi;
    time_t now;
    double tdiff;
    char time_str[40];
    char *timestamp_str, *sample_str, user[16];
    struct tm *sample_time;
    int int_count=0;
    size_t host_count=0;
    static int timeout = 0;
    orcm_sensor_hosts_t *cur_host, *host, *nxt;
    cur_host = current_host_configuration;

    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                            "%s sensor ipmi : collect_sample: called",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));


    if (mca_sensor_ipmi_component.test) {
        /* just send the test vector */
        bptr = &test_vector;
        opal_dss.pack(&sampler->bucket, &bptr, 1, OPAL_BUFFER);
        return;
    }
    if(disable_ipmi == 1)
        return;
    opal_output_verbose(2, orcm_sensor_base_framework.framework_output,
        "========================SAMPLE: %d===============================", count_log);

    /* prep the buffer to collect the data */
    OBJ_CONSTRUCT(&data, opal_buffer_t);
    /* pack our component name - 1*/
    ipmi = strdup("ipmi");
    if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &ipmi, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&data);
        return;
    }
    free(ipmi);

    if(count_log == 0 && timeout < 3)  /* The first time Sample is called, it shall retrieve/sample just the LAN credentials and pack it. */
    {
        /* Verify if user has root privileges, if not do not try to read BMC Credentials*/
        getlogin_r(user, 16);
        if(geteuid() != 0) {
            orte_show_help("help-orcm-sensor-ipmi.txt", "ipmi-not-superuser",
                           true, orte_process_info.nodename, user);
            timeout = 3;
            return;
        }
        timeout++;
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "First Sample: Packing Credentials");

        /* pack the numerical identifier for number of nodes*/
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &host_count, 1, OPAL_INT))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /* get the sample time */
        now = time(NULL);
        tdiff = difftime(now, last_sample);
        /* pass the time along as a simple string */
        sample_time = localtime(&now);
        if (NULL == sample_time) {
            ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
            return;
        }
        strftime(time_str, sizeof(time_str), "%F %T%z", sample_time);
        asprintf(&timestamp_str, "%s", time_str);

        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "***** TimeStamp: %s",timestamp_str);

        /* Pack the Sample Time - 1b */
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &timestamp_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }
        free(timestamp_str);

        /* Pack our node name - 2a*/
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &orte_process_info.nodename, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

//        rc = orcm_sensor_ipmi_get_bmc_cred(&cur_host);
        if(ORCM_SUCCESS != rc)
        {
            opal_output(0, "Retry : %d", timeout);
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /* Pack the host's IP Address - 3a*/
        sample_str = (char *)&cur_host->capsule.node.host_ip;
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /* Pack the BMC IP Address - 4a*/
        sample_str = (char *)&cur_host->capsule.node.bmc_ip;
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "Packing BMC IP: %s",sample_str);

        /* Pack the Baseboard Manufacture Date - 5a*/
        if (NULL == cur_host->capsule.prop.baseboard_manuf_date) {
            sample_str = "Board Manuf Date n/a";
        }
        else {
            sample_str = (char *)&cur_host->capsule.prop.baseboard_manuf_date;
        }
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /* Pack the Baseboard Manufacturer Name - 6a*/
        if (NULL == cur_host->capsule.prop.baseboard_manufacturer) {
            sample_str = "Board Manuf n/a";
        }
        else {
            sample_str = (char *)&cur_host->capsule.prop.baseboard_manufacturer;
        }
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /* Pack the Baseboard Product Name - 7a*/
        if (NULL == cur_host->capsule.prop.baseboard_name) {
            sample_str = "Board Name n/a";
        }
        else {
            sample_str = (char *)&cur_host->capsule.prop.baseboard_name;
        }
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /* Pack the Baseboard Serial Number - 8a*/
        if (NULL == cur_host->capsule.prop.baseboard_serial) {
            sample_str = "Board Serial n/a";
        }
        else {
            sample_str = (char *)&cur_host->capsule.prop.baseboard_serial;
        }
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /* Pack the Baseboard Part Number - 9a*/
        if (NULL == cur_host->capsule.prop.baseboard_part) {
            sample_str = "Board Part n/a";
        }
        else {
            sample_str = (char *)&cur_host->capsule.prop.baseboard_part;
        }
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /* Pack the buffer, to pass to heartbeat - FINAL */
        bptr = &data;
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&sampler->bucket, &bptr, 1, OPAL_BUFFER))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }
        if(!ORTE_PROC_IS_AGGREGATOR)
        {
            opal_output(0,"PROC_IS_COMPUTE_DAEMON");
            disable_ipmi = 1;
        } else {
            opal_output(0,"PROC_IS_AGGREGATOR");
        }

        return;
    } /* End packing BMC credentials*/

    /* Begin sampling known nodes from here */
    host_count = opal_list_get_size(&sensor_active_hosts);
    if (0 == host_count) {
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "No IPMI Device available for sampling");
        OBJ_DESTRUCT(&data);
        return;
    }
    /* pack the numerical identifier for number of nodes*/
    if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &host_count, 1, OPAL_INT))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&data);
        return;
    }

    /* Loop through each host from the host list*/
    OPAL_LIST_FOREACH_SAFE(host, nxt, &sensor_active_hosts, orcm_sensor_hosts_t) {
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "Scanning metrics from node: %s",host->capsule.node.name);
        /* Clear all memory for the ipmi_capsule */
        memset(&(host->capsule.prop), '\0', sizeof(host->capsule.prop));
        /* Enable/Disable the property/metric to be sampled */
        host->capsule.capability[BMC_REV]         = 1;
        host->capsule.capability[IPMI_VER]        = 1;
        host->capsule.capability[SYS_POWER_STATE] = 1;
        host->capsule.capability[DEV_POWER_STATE] = 1;

        /* If the bmc username was passed as an mca parameter, set it. */
        if (NULL != mca_sensor_ipmi_component.bmc_username) {
            strncpy(host->capsule.node.user, mca_sensor_ipmi_component.bmc_username, sizeof(mca_sensor_ipmi_component.bmc_username));
        }

        /* If not, set it to root by default. */
        else {
            strncpy(host->capsule.node.user, "root", sizeof("root"));
        }

        /*
        If the bmc password was passed as an mca parameter, set it.
        Otherwise, leave it as null.
        */
        if (NULL != mca_sensor_ipmi_component.bmc_password) {
            strncpy(host->capsule.node.pasw, mca_sensor_ipmi_component.bmc_password,
                    sizeof(host->capsule.node.pasw)-1);
        }

        host->capsule.node.auth = IPMI_SESSION_AUTHTYPE_PASSWORD;
        host->capsule.node.priv = IPMI_PRIV_LEVEL_ADMIN;
        host->capsule.node.ciph = 3; /* Cipher suite No. 3 */

        /* Running a sample for a Node */
        orcm_sensor_ipmi_exec_call(&host->capsule);

        /* get the sample time */
        now = time(NULL);
        tdiff = difftime(now, last_sample);
        /* pass the time along as a simple string */
        sample_time = localtime(&now);
        if (NULL == sample_time) {
            ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
            return;
        }
        strftime(time_str, sizeof(time_str), "%F %T%z", sample_time);
        asprintf(&timestamp_str, "%s", time_str);

        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "***** TimeStamp: %s",timestamp_str);
        /* Pack the Sample Time - 2 */
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &timestamp_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }
        free(timestamp_str);
 
        /* Pack the nodeName - 3 */
        sample_str = (char *)&host->capsule.node.name;
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "***** NodeName: %s",sample_str);
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /*  Pack the BMC FW Rev  - 4 */
        sample_str = (char *)&host->capsule.prop.bmc_rev;
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "***** bmcrev: %s",sample_str);
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /*  Pack the IPMI VER - 5 */
        sample_str = (char *)&host->capsule.prop.ipmi_ver;
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "***** IPMIVER: %s",sample_str);
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /*  Pack the Manufacturer ID - 6 */
        sample_str = (char *)&host->capsule.prop.man_id;
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "***** MANUF-ID: %s",sample_str);
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /*  Pack the System Power State - 7 */
        sample_str = (char *)&host->capsule.prop.sys_power_state;
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "***** SYS_PSTATE: %s",sample_str);
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /*  Pack the Device Power State - 8 */
        sample_str = (char *)&host->capsule.prop.dev_power_state;
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "***** DEV_PSTATE: %s",sample_str);
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&data);
            return;
        }

        /* Pack the total Number of Metrics Sampled */
        if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &host->capsule.prop.total_metrics, 1, OPAL_UINT))) {
                ORTE_ERROR_LOG(rc);
                OBJ_DESTRUCT(&data);
                return;
            }

        /*  Pack the non-string metrics and their units - 11-> END */
        for(int count_metrics=0;count_metrics<host->capsule.prop.total_metrics;count_metrics++)
        {
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "***** %s: %f",host->capsule.prop.metric_label[count_metrics], host->capsule.prop.collection_metrics[count_metrics]);

            sample_str = (char *)&host->capsule.prop.metric_label[count_metrics];
            if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                OBJ_DESTRUCT(&data);
                return;
            }

            if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &host->capsule.prop.collection_metrics[count_metrics], 1, OPAL_FLOAT))) {
                ORTE_ERROR_LOG(rc);
                OBJ_DESTRUCT(&data);
                return;
            }

            sample_str = (char *)&host->capsule.prop.collection_metrics_units[count_metrics];
            if (OPAL_SUCCESS != (rc = opal_dss.pack(&data, &sample_str, 1, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                OBJ_DESTRUCT(&data);
                return;
            }
        }
    }
    /* Pack the list into a buffer and pass it onto heartbeat */
    bptr = &data;
    if (OPAL_SUCCESS != (rc = opal_dss.pack(&sampler->bucket, &bptr, 1, OPAL_BUFFER))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&data);
        return;
    }

    OBJ_DESTRUCT(&data);
    last_sample = now;
    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
        "Total nodes sampled: %d",int_count);
    /* this is currently a no-op */
    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                        "IPMI sensors just got implemented! ----------- ;)");
}

static void mycleanup(int dbhandle, int status,
                      opal_list_t *kvs, void *cbdata)
{
    OPAL_LIST_RELEASE(kvs);
    if (ORTE_SUCCESS != status) {
        log_enabled = false;
    }
}

static void ipmi_log(opal_buffer_t *sample)
{
    char *hostname, *sampletime, *sample_item, *sample_name, *sample_unit, *key_unit;
    char nodename[64], hostip[16], bmcip[16], baseboard_manuf_date[11], baseboard_manufacturer[30], baseboard_name[16], baseboard_serial[16], baseboard_part[16];
    float float_item;
    unsigned uint_item;
    int rc;
    int32_t n;
    opal_list_t *vals;
    opal_value_t *kv;
    int host_count;
    if (!log_enabled) {
        return;
    }
    if(disable_ipmi == 1)
        return;
    opal_output_verbose(2, orcm_sensor_base_framework.framework_output,
        "----------------------LOG: %d----------------------------", count_log);
    count_log++;
    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
        "Count Log: %d", count_log);

    /* Unpack the host_count identifer */
    n=1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &host_count, &n, OPAL_INT))) {
        ORTE_ERROR_LOG(rc);
        return;
    } else {
        if(host_count==0) {
            /*New Node is getting added */

            /* sample time - 1b */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sampletime, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            if (NULL == sampletime) {
                ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
                return;
            }

            /* Unpack the node_name - 2 */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &hostname, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            strncpy(nodename,hostname,strlen(hostname)+1);
            opal_output(0,"IPMI_LOG -> Node %s not found; Logging credentials", hostname);
            free(hostname);

            /* Unpack the host_ip - 3a */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &hostname, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            if (NULL == hostname) {
                ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
                return;
            }
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "Unpacked host_ip(3a): %s",hostname);
            strncpy(hostip,hostname,strlen(hostname)+1);
            free(hostname);

            /* Unpack the bmcip - 4a */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &hostname, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            if (NULL == hostname) {
                ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
                return;
            }
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "Unpacked BMC_IP(4a): %s",hostname);
            strncpy(bmcip,hostname,strlen(hostname)+1);
            free(hostname);

            /* Unpack the Baseboard Manufacture Date - 5a */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_item, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            if (NULL == sample_item) {
                ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
                return;
            }
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "Unpacked Baseboard Manufacture Date(5a): %s", sample_item);
            strncpy(baseboard_manuf_date,sample_item,(sizeof(baseboard_manuf_date)-1));
            baseboard_manuf_date[sizeof(baseboard_manuf_date)-1] = '\0';
            free(sample_item);

            /* Unpack the Baseboard Manufacturer Name - 6a */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_item, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            if (NULL == sample_item) {
                ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
                return;
            }
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "Unpacked Baseboard Manufacturer Name(6a): %s", sample_item);
            strncpy(baseboard_manufacturer,sample_item,(sizeof(baseboard_manufacturer)-1));
            baseboard_manufacturer[sizeof(baseboard_manufacturer)-1] = '\0';
            free(sample_item);

            /* Unpack the Baseboard Product Name - 7a */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_item, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            if (NULL == sample_item) {
                ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
                return;
            }
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "Unpacked Baseboard Product Name(7a): %s", sample_item);
            strncpy(baseboard_name,sample_item,(sizeof(baseboard_name)-1));
            baseboard_name[sizeof(baseboard_name)-1] = '\0';
            free(sample_item);

            /* Unpack the Baseboard Serial Number - 8a */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_item, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            if (NULL == sample_item) {
                ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
                return;
            }
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "Unpacked Baseboard Serial Number(8a): %s", sample_item);
            strncpy(baseboard_serial,sample_item,(sizeof(baseboard_serial)-1));
            baseboard_serial[sizeof(baseboard_serial)-1] = '\0';
            free(sample_item);

            /* Unpack the Baseboard Part Number - 9a */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_item, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            if (NULL == sample_item) {
                ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
                return;
            }
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "Unpacked Baseboard Part Number(9a): %s", sample_item);
            strncpy(baseboard_part,sample_item,(sizeof(baseboard_part)-1));
            baseboard_part[sizeof(baseboard_part)-1] = '\0';
            free(sample_item);

            /* Add the node only if it has not been added previously, for the 
             * off chance that the compute node daemon was started once before,
             * and after running for sometime was killed
             * VINFIX: Eventually, this node which is already present and is 
             * re-started has to be removed first, and then added again afresh,
             * just so that we update our list with the latest credentials
             */
            if(ORCM_ERR_NOT_FOUND == orcm_sensor_ipmi_found(nodename, &sensor_active_hosts))
            {
                if(ORCM_SUCCESS != orcm_sensor_ipmi_addhost(nodename, hostip, bmcip, &sensor_active_hosts)) /* Add the node to the slave list of the aggregator */
                {
                    opal_output(0,"Unable to add the new host! Try restarting ORCM");
                    orte_show_help("help-orcm-sensor-ipmi.txt", "ipmi-addhost-fail", 
                           true, orte_process_info.nodename, nodename);
                    return;
                }
            } else {
                opal_output(0,"Node already populated; Not going be added again");
            }
            /* Log the static information to database */
            /* @VINFIX: Currently will log into the same database as sensor data
             * But will eventually get moved to a different database (read
             * Inventory)
             */
            vals = OBJ_NEW(opal_list_t);

            kv = OBJ_NEW(opal_value_t);
            kv->key = strdup("ctime");
            kv->type = OPAL_STRING;
            kv->data.string = strdup(sampletime);
            opal_list_append(vals, &kv->super);
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "UnPacked cTime: %s", sampletime);
            free(sampletime);

            kv = OBJ_NEW(opal_value_t);
            kv->key = strdup("nodename");
            kv->type = OPAL_STRING;
            kv->data.string = strdup(nodename);
            opal_list_append(vals, &kv->super);
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "UnPacked NodeName: %s", nodename);

             /* Add Baseboard manufacture date */
            kv = OBJ_NEW(opal_value_t);
            kv->key = strdup("BBmanuf_date");
            kv->type = OPAL_STRING;
            kv->data.string = strdup(baseboard_manuf_date);
            opal_list_append(vals, &kv->super);
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "UnPacked NodeName: %s", nodename);

             /* Add Baseboard manufacturer name */
            kv = OBJ_NEW(opal_value_t);
            kv->key = strdup("BBmanuf");
            kv->type = OPAL_STRING;
            kv->data.string = strdup(baseboard_manufacturer);
            opal_list_append(vals, &kv->super);
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "UnPacked NodeName: %s", nodename);

             /* Add Baseboard product name */
            kv = OBJ_NEW(opal_value_t);
            kv->key = strdup("BBname");
            kv->type = OPAL_STRING;
            kv->data.string = strdup(baseboard_name);
            opal_list_append(vals, &kv->super);
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "UnPacked NodeName: %s", nodename);

            /* Add Baseboard serial number */
            kv = OBJ_NEW(opal_value_t);
            kv->key = strdup("BBserial");
            kv->type = OPAL_STRING;
            kv->data.string = strdup(baseboard_serial);
            opal_list_append(vals, &kv->super);
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "UnPacked NodeName: %s", nodename);

            /* Add Baseboard part number */
            kv = OBJ_NEW(opal_value_t);
            kv->key = strdup("BBpart");
            kv->type = OPAL_STRING;
            kv->data.string = strdup(baseboard_part);
            opal_list_append(vals, &kv->super);
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "UnPacked NodeName: %s", nodename);

            /* Send the unpacked data for one Node */
            /* store it */
            if (0 <= orcm_sensor_base.dbhandle) {
                orcm_db.store(orcm_sensor_base.dbhandle, "ipmi", vals, mycleanup, NULL);
            } else {
                OPAL_LIST_RELEASE(vals);
            }
            return;
        } else {
            opal_output_verbose(2, orcm_sensor_base_framework.framework_output,
                "IPMI_LOG -> Node Found; Logging metrics");
        }
    }
    opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
        "Total Samples to be unpacked: %d", host_count);

    /* START UNPACKING THE DATA and Store it in a opal_list_t item. */
    for(int count = 0; count < host_count; count++)
    {
        vals = OBJ_NEW(opal_list_t);

        /* sample time - 2 */
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sampletime, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        if (NULL == sampletime) {
            ORTE_ERROR_LOG(OPAL_ERR_BAD_PARAM);
            return;
        }
        kv = OBJ_NEW(opal_value_t);
        kv->key = strdup("ctime");
        kv->type = OPAL_STRING;
        kv->data.string = strdup(sampletime);
        opal_list_append(vals, &kv->super);
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "UnPacked cTime: %s", sampletime);
        free(sampletime);

        /* Unpack the node_name - 3 */
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &hostname, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        kv = OBJ_NEW(opal_value_t);
        kv->key = strdup("nodename");
        kv->type = OPAL_STRING;
        kv->data.string = strdup(hostname);
        opal_list_append(vals, &kv->super);
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "UnPacked NodeName: %s", hostname);
        strncpy(nodename,hostname,strlen(hostname)+1);
        free(hostname);

        /* BMC FW REV - 4 */
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_item, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        kv = OBJ_NEW(opal_value_t);
        kv->key = strdup("bmcfwrev");
        kv->type = OPAL_STRING;
        kv->data.string = strdup(sample_item);
        opal_list_append(vals, &kv->super);
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "UnPacked bmcfwrev: %s", sample_item);
        free(sample_item);

        /* IPMI VER - 5 */
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_item, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        kv = OBJ_NEW(opal_value_t);
        kv->key = strdup("ipmiver");
        kv->type = OPAL_STRING;
        kv->data.string = strdup(sample_item);
        opal_list_append(vals, &kv->super);
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "UnPacked ipmiver: %s", sample_item);
        free(sample_item);

        /* Manufacturer ID - 6 */
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_item, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        kv = OBJ_NEW(opal_value_t);
        kv->key = strdup("manufacturer_id");
        kv->type = OPAL_STRING;
        kv->data.string = strdup(sample_item);
        opal_list_append(vals, &kv->super);
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "UnPacked MANUF-ID: %s", sample_item);
        free(sample_item);

        /* System Power State - 7 */
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_item, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        kv = OBJ_NEW(opal_value_t);
        kv->key = strdup("sys_power_state");
        kv->type = OPAL_STRING;
        kv->data.string = strdup(sample_item);
        opal_list_append(vals, &kv->super);
        free(sample_item);

        /* Device Power State - 8 */
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_item, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        kv = OBJ_NEW(opal_value_t);
        kv->key = strdup("dev_power_state");
        kv->type = OPAL_STRING;
        kv->data.string = strdup(sample_item);
        opal_list_append(vals, &kv->super);
        opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
            "UnPacked DEV_PSTATE: %s", sample_item);
        free(sample_item);

        /* Total BMC sensor Metrics sampled - 9 (Not necessary for db_store) */
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &uint_item, &n, OPAL_UINT))) {
            ORTE_ERROR_LOG(rc);
            return;
        }

        /* Log All non-string metrics here */
        for(unsigned int count_metrics=0;count_metrics<uint_item;count_metrics++)
        {
            key_unit = NULL; /* reset pointers */
            /* Metric Name */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_name, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            
            /* Metric Value*/
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &float_item, &n, OPAL_FLOAT))) {
                ORTE_ERROR_LOG(rc);
                return;
            }

            /* Metric Units */
            n=1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sample_unit, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }

            /* allocate memory for "sample_name"+":"+"sample_unit"+"\0" */
            key_unit = (char*) malloc(strlen(sample_name)+strlen(sample_unit)+2);
            if (key_unit == NULL)
            {
                ORTE_ERROR_LOG(ORCM_ERR_OUT_OF_RESOURCE);
                free(sample_name);
                free(sample_unit);
                continue;
            }
            strcpy(key_unit,sample_name);
            if(strlen(sample_unit) > 0)
            {
                strcat(key_unit,":");
                strcat(key_unit,sample_unit);
            }

            kv = OBJ_NEW(opal_value_t);
            kv->key = strdup(key_unit);
            kv->type = OPAL_FLOAT;
            kv->data.fval = float_item;
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "UnPacked %s: %f", key_unit, float_item);
            opal_list_append(vals, &kv->super);
            opal_output_verbose(5, orcm_sensor_base_framework.framework_output,
                "PACKED DATA: %s:%f", kv->key, kv->data.fval);
            free(sample_name);
            free(sample_unit);
            free(key_unit);
        }
        /* Send the unpacked data for one Node */
        /* store it */
        if (0 <= orcm_sensor_base.dbhandle) {
            orcm_db.store(orcm_sensor_base.dbhandle, "ipmi", vals, mycleanup, NULL);
        } else {
            OPAL_LIST_RELEASE(vals);
        }
    }
}

static void generate_test_vector(opal_buffer_t *v)
{

}

static void generate_test_vector_inv(opal_buffer_t *inventory_snapshot)
{
    char *comp;
    unsigned int tot_items = 5;
    int rc;
    if(NULL != inventory_snapshot)
    {
    
        comp = strdup("ipmi");
        if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        free(comp);
        if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &tot_items, 1, OPAL_UINT))) {
            ORTE_ERROR_LOG(rc);
            return;
        }

        while(tot_items > 0)
        {
            tot_items--;
            comp = ipmi_inv_tv[tot_items][0];
            if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            comp = ipmi_inv_tv[tot_items][1];
            if (OPAL_SUCCESS != (rc = opal_dss.pack(inventory_snapshot, &comp, 1, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
        }
    }
}

void orcm_sensor_ipmi_get_system_power_state(uchar in, char* str)
{
    char in_r = in & 0x7F;
    switch(in_r) {
        case 0x0:   strncpy(str,"S0/G0",sizeof("S0/G0")); break;
        case 0x1:   strncpy(str,"S1",sizeof("S1")); break;
        case 0x2:   strncpy(str,"S2",sizeof("S2")); break;
        case 0x3:   strncpy(str,"S3",sizeof("S3")); break;
        case 0x4:   strncpy(str,"S4",sizeof("S4")); break;
        case 0x5:   strncpy(str,"S5/G2",sizeof("S5/G2")); break;
        case 0x6:   strncpy(str,"S4/S5",sizeof("S4/S5")); break;
        case 0x7:   strncpy(str,"G3",sizeof("G3")); break;
        case 0x8:   strncpy(str,"sleeping",sizeof("sleeping")); break;
        case 0x9:   strncpy(str,"G1 sleeping",sizeof("G1 sleeping")); break;
        case 0x0A:  strncpy(str,"S5 override",sizeof("S5 override")); break;
        case 0x20:  strncpy(str,"Legacy On",sizeof("Legacy On")); break;
        case 0x21:  strncpy(str,"Legacy Off",sizeof("Legacy Off")); break;
        case 0x2A:  strncpy(str,"Unknown",sizeof("Unknown")); break;
        default:    strncpy(str,"Illegal",sizeof("Illegal")); break;
    }
}
void orcm_sensor_ipmi_get_device_power_state(uchar in, char* str)
{
    char in_r = in & 0x7F;
    switch(in_r) {
        case 0x0:   strncpy(str,"D0",sizeof("D0")); break;
        case 0x1:   strncpy(str,"D1",sizeof("D1")); break;
        case 0x2:   strncpy(str,"D2",sizeof("D2")); break;
        case 0x3:   strncpy(str,"D3",sizeof("D3")); break;
        case 0x4:   strncpy(str,"Unknown",sizeof("Unknown")); break;
        default:    strncpy(str,"Illegal",sizeof("Illegal")); break;
    }
}

void orcm_sensor_ipmi_exec_call(ipmi_capsule_t *cap)
{
    char addr[16];
    int ret = 0;
    unsigned char idata[4], rdata[256];
    unsigned char ccode;
    int rlen = 256;
    char fdebug = 0;
    /* ipmi_capsule_t *cap = (ipmi_capsule_t*)(thread_param); */
    device_id_t devid;
    acpi_power_state_t pwr_state;

    char tag[17];
    unsigned char snum = 0;
    unsigned char reading[4];       /* Stores the individual sensor reading */
    double val;
    char *typestr;                  /* Stores the individual sensor unit */
    unsigned short int id = 0;
    unsigned char sdrbuf[SDR_SZ];
    unsigned char *sdrlist;
    char *error_string;
    int sensor_count = 0;

    char sys_pwr_state_str[16], dev_pwr_state_str[16];
    char test[16], test1[16];

    memset(rdata,0xff,256);
    memset(idata,0xff,4);
    if (cap->capability[BMC_REV] & cap->capability[IPMI_VER])
    {
        ret = set_lan_options(cap->node.bmc_ip, cap->node.user, cap->node.pasw, cap->node.auth, cap->node.priv, cap->node.ciph, &addr, 16);
        if(0 == ret)
        {
            ret = ipmi_cmd_mc(GET_DEVICE_ID, idata, 0, rdata, &rlen, &ccode, fdebug);
            if(0 == ret)
            {
                ipmi_close();
                memcpy(&devid.raw, rdata, sizeof(devid));

                /*  Pack the BMC FW Rev */
                sprintf(test,"%x", devid.bits.fw_rev_1&0x7F);
                sprintf(test1,"%x", devid.bits.fw_rev_2&0xFF);
                strcat(test,".");
                strcat(test,test1);
                strncpy(cap->prop.bmc_rev, test, sizeof(test));

                /*  Pack the IPMI VER */
                sprintf(test,"%x", devid.bits.ipmi_ver&0xF);
                sprintf(test1,"%x", devid.bits.ipmi_ver&0xF0);
                strcat(test,".");
                strcat(test,test1);
                strncpy(cap->prop.ipmi_ver, test, sizeof(test));

                /*  Pack the Manufacturer ID */
                sprintf(test,"%02x", devid.bits.manufacturer_id[1]);
                sprintf(test1,"%02x", devid.bits.manufacturer_id[0]);
                strcat(test,test1);
                strncpy(cap->prop.man_id, test, sizeof(test));
            }

            else {
                /*disable_ipmi = 1;*/
                error_string = decode_rv(ret);
                orte_show_help("help-orcm-sensor-ipmi.txt", "ipmi-cmd-mc-fail",
                           true, orte_process_info.nodename, 
                           orte_process_info.nodename, cap->node.bmc_ip,
                           cap->node.user, cap->node.pasw, cap->node.auth,
                           cap->node.priv, cap->node.ciph, error_string);
            }
        }

        else {
            error_string = decode_rv(ret);
            orte_show_help("help-orcm-sensor-ipmi.txt", "ipmi-set-lan-fail",
                           true, orte_process_info.nodename,
                           orte_process_info.nodename, cap->node.bmc_ip,
                           cap->node.user, cap->node.pasw, cap->node.auth,
                           cap->node.priv, cap->node.ciph, error_string);
        }
    }

    if  (cap->capability[SYS_POWER_STATE] & cap->capability[DEV_POWER_STATE])
    {
        memset(rdata,0xff,256);
        memset(idata,0xff,4);
        ret = set_lan_options(cap->node.bmc_ip, cap->node.user, cap->node.pasw, cap->node.auth, cap->node.priv, cap->node.ciph, &addr, 16);
        if(0 == ret)
        {
            ret = ipmi_cmd_mc(GET_ACPI_POWER, idata, 0, rdata, &rlen, &ccode, fdebug);
            if(0 == ret)
            {
                ipmi_close();
                memcpy(&pwr_state.raw, rdata, sizeof(pwr_state));
                orcm_sensor_ipmi_get_system_power_state(pwr_state.bits.sys_power_state, sys_pwr_state_str);
                orcm_sensor_ipmi_get_device_power_state(pwr_state.bits.dev_power_state, dev_pwr_state_str);
                /* Copy all retrieved information in a global buffer */
                memcpy(cap->prop.sys_power_state,sys_pwr_state_str,sizeof(sys_pwr_state_str));
                memcpy(cap->prop.dev_power_state,dev_pwr_state_str,sizeof(dev_pwr_state_str));
            }

            else {
                error_string = decode_rv(ret);
                orte_show_help("help-orcm-sensor-ipmi.txt", "ipmi-cmd-mc-fail",
                           true, orte_process_info.nodename,
                           orte_process_info.nodename, cap->node.bmc_ip,
                           cap->node.user, cap->node.pasw, cap->node.auth,
                           cap->node.priv, cap->node.ciph, error_string);
            }
        }

        else {
            error_string = decode_rv(ret);
            orte_show_help("help-orcm-sensor-ipmi.txt", "ipmi-set-lan-fail",
                           true, orte_process_info.nodename,
                           orte_process_info.nodename, cap->node.bmc_ip,
                           cap->node.user, cap->node.pasw, cap->node.auth,
                           cap->node.priv, cap->node.ciph, error_string);
        }
    }

    /* BEGIN: Gathering SDRs */
    /* @VINFIX : NOTE!!!
    * Currently Read sensors uses the additional functionality implemented in isensor.h & ievent.h
    * These files are not part of the libipmiutil and needs to be build along with the IPMI Plugin
    * No licensing issues since they are released under FreeBSD
    */ 
    memset(rdata,0xff,256);
    memset(idata,0xff,4);
    ret = set_lan_options(cap->node.bmc_ip, cap->node.user, cap->node.pasw, cap->node.auth, cap->node.priv, cap->node.ciph, &addr, 16);
    if(ret)
    {
        error_string = decode_rv(ret);
        orte_show_help("help-orcm-sensor-ipmi.txt", "ipmi-set-lan-fail",
                           true, orte_process_info.nodename,
                           orte_process_info.nodename, cap->node.bmc_ip,
                           cap->node.user, cap->node.pasw, cap->node.auth,
                           cap->node.priv, cap->node.ciph, error_string);
        return;
    } else {
        ret = get_sdr_cache(&sdrlist);
        if (ret) {
            error_string = decode_rv(ret);
            orte_show_help("help-orcm-sensor-ipmi.txt", "ipmi-get-sdr-fail",
                           true, orte_process_info.nodename,
                           orte_process_info.nodename, cap->node.bmc_ip,
                           cap->node.user, cap->node.pasw, cap->node.auth,
                           cap->node.priv, cap->node.ciph, error_string);
            return;
        } else {
            while(find_sdr_next(sdrbuf,sdrlist,id) == 0)
            {
                id = sdrbuf[0] + (sdrbuf[1] << 8); /* this SDR id */
                if (sdrbuf[3] != 0x01) continue; /* full SDR */
                strncpy(tag,(char *)&sdrbuf[48],16);
                tag[16] = 0;
                snum = sdrbuf[7];
                ret = GetSensorReading(snum, sdrbuf, reading);
                if (ret == 0)
                {
                    val = RawToFloat(reading[0], sdrbuf);
                    typestr = get_unit_type( sdrbuf[20], sdrbuf[21], sdrbuf[22],0);
                    if(orcm_sensor_ipmi_label_found(tag))
                    {
                        /*opal_output(0, "Found Sensor Label matching:%s",tag);*/
                        /*  Pack the Sensor Metric */
                        cap->prop.collection_metrics[sensor_count]=val;
                        strncpy(cap->prop.collection_metrics_units[sensor_count],typestr,sizeof(cap->prop.collection_metrics_units[sensor_count]));
                        strncpy(cap->prop.metric_label[sensor_count],tag,sizeof(cap->prop.metric_label[sensor_count]));
                        sensor_count++;
                    } else if(NULL!=mca_sensor_ipmi_component.sensor_group)
                    {
                        if(NULL!=strcasestr(tag, mca_sensor_ipmi_component.sensor_group))
                        {
                            /*opal_output(0, "Found Sensor Label '%s' matching group:%s", tag, mca_sensor_ipmi_component.sensor_group);*/
                            /*  Pack the Sensor Metric */
                            cap->prop.collection_metrics[sensor_count]=val;
                            strncpy(cap->prop.collection_metrics_units[sensor_count],typestr,sizeof(cap->prop.collection_metrics_units[sensor_count]));
                            strncpy(cap->prop.metric_label[sensor_count],tag,sizeof(cap->prop.metric_label[sensor_count]));
                            sensor_count++;
                        }
                    }
                    if (sensor_count == TOTAL_FLOAT_METRICS)
                    {
                        opal_output(0, "Max 'sensor' sampling reached for IPMI Plugin: %d",
                            sensor_count);
                        break;
                    }
                } else {
                    val = 0;
                    typestr = "na";
                    /*opal_output(0, "%04x: get sensor %x reading ret = %d\n",id,snum,ret);*/
                }
                memset(sdrbuf,0,SDR_SZ);
            }
            free_sdr_cache(sdrlist);
            cap->prop.total_metrics = sensor_count;
        }
        ipmi_close();
        /* End: gathering SDRs */
    }
}
