/*
 * @section LICENSE
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * 
 * @author: msune, akoepsel, tjungel, valvarez, 
 * 
 * @section DESCRIPTION 
 * 
 * GNU/Linux forwarding_module dispatching routines. This file contains primary AFA driver hooks
 * for CMM to call forwarding module specific functions (e.g. bring up port, or create logical switch).
 * Openflow version dependant hooks are under openflow/ folder. 
*/


#include <stdio.h>
#include <rofl/datapath/hal/driver.h>
#include <rofl/common/utils/c_logger.h>
#include <rofl/datapath/hal/cmm.h>
#include <rofl/datapath/pipeline/platform/memory.h>
#include <rofl/datapath/pipeline/physical_switch.h>
#include <rofl/datapath/pipeline/openflow/openflow1x/of1x_switch.h>
#include "../io/bufferpool.h"
#include "../bg_taskmanager.h"

#include "../io/iface_utils.h"
#include "../ezappliance/ez_packet_channel.h"

//only for Test
#include <stdlib.h>
#include <string.h>
#include <rofl/datapath/pipeline/openflow/of_switch.h>
#include <rofl/datapath/pipeline/common/datapacket.h>


#define NUM_ELEM_INIT_BUFFERPOOL 2048 //This is cache for fast port addition

//Driver static info
#define EZ_CODE_NAME "ezappliance"
#define EZ_VERSION VERSION 
#define EZ_DESC \
"EZappliance driver.\n\nEZappliance driver is controlling EZchip NP-3 network processor based programmable devices."

#define EZ_USAGE  "EZ Proxy IP address required" 
#define EZ_EXTRA_PARAMS "EZ Proxy IP address" 


using namespace xdpd::gnu_linux;

/*
* @name    hal_driver_init
* @brief   Initializes driver. Before using the HAL_DRIVER routines, higher layers must allow driver to initialize itself
* @ingroup driver_management
*/
hal_result_t hal_driver_init(const char* extra_params){

	ROFL_INFO("[AFA] Initializing EZappliance forwarding module...\n");
    ROFL_INFO("[AFA] Extra params is %s\n", extra_params);
        
	//Init the ROFL-PIPELINE phyisical switch
	if(physical_switch_init() != ROFL_SUCCESS)
		return HAL_FAILURE;

	//create bufferpool
	bufferpool::init();
        
        //Initialize packet channel to EZ
        // extra_params contains IP address of EZ Proxy
        if(launch_ez_packet_channel(extra_params) != ROFL_SUCCESS){
                return HAL_FAILURE;
        }
        
	if(discover_physical_ports() != ROFL_SUCCESS)
		return HAL_FAILURE;

	//Initialize Background Tasks Manager
	if(launch_background_tasks_manager() != ROFL_SUCCESS){
		return HAL_FAILURE;
	}
	return HAL_SUCCESS; 
}

/**
* @name    hal_driver_get_info
* @brief   Get the information of the driver (code-name, version, usage...)
* @ingroup driver_management
*/
void hal_driver_get_info(driver_info_t* info){
	//Fill-in driver_info_t
	strncpy(info->code_name, EZ_CODE_NAME, DRIVER_CODE_NAME_MAX_LEN);
	strncpy(info->version, EZ_VERSION, DRIVER_VERSION_MAX_LEN);
	strncpy(info->description, EZ_DESC, DRIVER_DESCRIPTION_MAX_LEN);
	strncpy(info->usage, EZ_USAGE, DRIVER_USAGE_MAX_LEN);
	strncpy(info->extra_params, EZ_EXTRA_PARAMS, DRIVER_EXTRA_PARAMS_MAX_LEN);
}

/*
* @name    hal_driver_destroy
* @brief   Destroy driver state. Allows platform state to be properly released. 
* @ingroup driver_management
*/
hal_result_t hal_driver_destroy(){
    
    ROFL_DEBUG("[AFA] driver_destroy\n");

    unsigned int i, max_switches;
    of_switch_t** switch_list;
    
    //Stop 
    stop_ez_packet_channel();
    
    //Stop all logical switch instances (stop processing packets)
	switch_list = physical_switch_get_logical_switches(&max_switches);
	for(i=0;i<max_switches;++i){
		if(switch_list[i] != NULL){
			hal_driver_destroy_switch_by_dpid(switch_list[i]->dpid);
		}
	}
        
	//Stop the bg manager
	stop_background_tasks_manager();

	//Destroy interfaces
	destroy_ports();

	//Destroy physical switch (including ports)
	physical_switch_destroy();
	
	// destroy bufferpool
	bufferpool::destroy();
	
	ROFL_INFO("[AFA] EZappliance forwarding module destroyed.\n");
	
	return HAL_SUCCESS; 
}

/*
* Switch management functions
*/

/**
* @brief   Checks if an LSI with the specified dpid exists 
* @ingroup logical_switch_management
*/
bool hal_driver_switch_exists(uint64_t dpid){
	return physical_switch_get_logical_switch_by_dpid(dpid) != NULL;
}

/**
* @brief   Retrieve the list of LSIs dpids
* @ingroup logical_switch_management
* @retval  List of available dpids, which MUST be deleted using dpid_list_destroy().
*/
dpid_list_t* hal_driver_get_all_lsi_dpids(void){
	return physical_switch_get_all_lsi_dpids();  
}

/**
 * @name hal_driver_get_switch_snapshot_by_dpid 
 * @brief Retrieves a snapshot of the current state of a switch port, if the port name is found. The snapshot MUST be deleted using switch_port_destroy_snapshot()
 * @ingroup logical_switch_management
 * @retval  Pointer to of_switch_snapshot_t instance or NULL 
 */
of_switch_snapshot_t* hal_driver_get_switch_snapshot_by_dpid(uint64_t dpid){
	return physical_switch_get_logical_switch_snapshot(dpid);
}

/*
* @name    hal_driver_create_switch 
* @brief   Instruct driver to create an OF logical switch 
* @ingroup logical_switch_management
* @retval  Pointer to of_switch_t instance 
*/
hal_result_t hal_driver_create_switch(char* name, uint64_t dpid, of_version_t of_version, unsigned int num_of_tables, int* ma_list){
	
        ROFL_DEBUG("[AFA] driver_create_switch (name: %s, dpid: %d, tables: %d)\n", name, dpid, num_of_tables);
	of_switch_t* sw;
	
	sw = (of_switch_t*)of1x_init_switch(name, of_version, dpid, num_of_tables, (enum of1x_matching_algorithm_available*) ma_list);
	
    if(unlikely(!sw))
        return HAL_FAILURE;
        
	//Add switch to the bank	
	physical_switch_add_logical_switch(sw);
    
    // Add switch (with pipeline) to EZ-packet-channel
	set_lsw_for_ez_packet_channel(sw);
	return HAL_SUCCESS;
}


/*
* @name    hal_driver_get_switch_by_dpid 
* @brief   Retrieve the switch with the specified dpid  
* @ingroup logical_switch_management
* @retval  Pointer to of_switch_t instance or NULL 
*/
of_switch_t* hal_driver_get_switch_by_dpid(uint64_t dpid){
	
        ROFL_DEBUG("[AFA] driver_init (dpid: %d)\n", dpid);
	//Call directly the bank
	return physical_switch_get_logical_switch_by_dpid(dpid); 
}

/*
* @name    driver_destroy_switch_by_dpid 
* @brief   Instructs the driver to destroy the switch with the specified dpid 
* @ingroup logical_switch_management
*/
hal_result_t hal_driver_destroy_switch_by_dpid(const uint64_t dpid){

        ROFL_DEBUG("[AFA] driver_destroy_switch_by_dpid (dpid: %d)\n", dpid);
	unsigned int i;
	
	//Try to retrieve the switch
	of_switch_t* sw = physical_switch_get_logical_switch_by_dpid(dpid);
	
	if(!sw)
		return HAL_FAILURE;

	//Stop all ports and remove it from being scheduled by I/O first
	for(i=0;i<sw->max_ports;i++){

		if(sw->logical_ports[i].attachment_state == LOGICAL_PORT_STATE_ATTACHED && sw->logical_ports[i].port){

		}
	}
	
	//Detach ports from switch. Do not feed more packets to the switch
	if(physical_switch_detach_all_ports_from_logical_switch(sw)!=ROFL_SUCCESS)
		return HAL_FAILURE;
	
	//Remove switch from the switch bank
	if(physical_switch_remove_logical_switch(sw)!=ROFL_SUCCESS)
		return HAL_FAILURE;
	
	return HAL_SUCCESS;
}

/*
* Port management 
*/

/**
* @brief   Checks if a port with the specified name exists 
* @ingroup port_management 
*/
bool hal_driver_port_exists(const char *name){
	return physical_switch_get_port_by_name(name) != NULL; 
}

/**
* @brief   Retrieve the list of names of the available ports of the platform. You may want to 
* 	   call hal_driver_get_port_snapshot_by_name(name) to get more information of the port 
* @ingroup port_management
* @retval  List of available port names, which MUST be deleted using switch_port_name_list_destroy().
*/
switch_port_name_list_t* hal_driver_get_all_port_names(void){
	return physical_switch_get_all_port_names(); 
}

/**
 * @name hal_driver_get_port_by_name
 * @brief Retrieves a snapshot of the current state of a switch port, if the port name is found. The snapshot MUST be deleted using switch_port_destroy_snapshot()
 * @ingroup port_management
 */
switch_port_snapshot_t* hal_driver_get_port_snapshot_by_name(const char *name){
	return physical_switch_get_port_snapshot(name); 
}

/**
 * @name hal_driver_get_port_by_num
 * @brief Retrieves a snapshot of the current state of the port of the Logical Switch Instance with dpid at port_num, if exists. The snapshot MUST be deleted using switch_port_destroy_snapshot()
 * @ingroup port_management
 * @param dpid DatapathID 
 * @param port_num Port number
 */
switch_port_snapshot_t* hal_driver_get_port_snapshot_by_num(uint64_t dpid, unsigned int port_num){
	
	of_switch_t* lsw;
	
	lsw = physical_switch_get_logical_switch_by_dpid(dpid);
	if(!lsw)
		return NULL; 

	//Check if the port does exist.
	if(!port_num || port_num >= LOGICAL_SWITCH_MAX_LOG_PORTS || !lsw->logical_ports[port_num].port)
		return NULL;

	return physical_switch_get_port_snapshot(lsw->logical_ports[port_num].port->name); 
}


/*
* @name    hal_driver_attach_physical_port_to_switch
* @brief   Attemps to attach a system's port to switch, at of_port_num if defined, otherwise in the first empty OF port number.
* @ingroup management
*
* @param dpid Datapath ID of the switch to attach the ports to
* @param name Port name (system's name)
* @param of_port_num If *of_port_num is non-zero, try to attach to of_port_num of the logical switch, otherwise try to attach to the first available port and return the result in of_port_num
*/
hal_result_t hal_driver_attach_port_to_switch(uint64_t dpid, const char* name, unsigned int* of_port_num){

        ROFL_DEBUG("[AFA] driver_attach_port_to_switch (dpid: %d, name: %s)\n", dpid, name);
    
	switch_port_t* port;
    switch_port_snapshot_t* port_snapshot;
	of_switch_t* lsw;

	//Check switch existance
	lsw = physical_switch_get_logical_switch_by_dpid(dpid);
	if(!lsw){
		return HAL_FAILURE;
	}
	
	//Check if the port does exist
	port = physical_switch_get_port_by_name(name);
	if(!port)
		return HAL_FAILURE;

	//Update pipeline state
	if(*of_port_num == 0){
		//no port specified, we assign the first available
		if(physical_switch_attach_port_to_logical_switch(port,lsw,of_port_num) == ROFL_FAILURE){
			assert(0);
			return HAL_FAILURE;
		}
	}else{

		if(physical_switch_attach_port_to_logical_switch_at_port_num(port,lsw,*of_port_num) == ROFL_FAILURE){
			assert(0);
			return HAL_FAILURE;
		}
	}

	//notify port attached
    port_snapshot = physical_switch_get_port_snapshot(port->name);
	if(hal_cmm_notify_port_add(port_snapshot)!=HAL_SUCCESS){
		//return HAL_FAILURE; //Ignore
	}
	
	return HAL_SUCCESS;
}

/**
* @name    hal_driver_connect_switches
* @brief   Attemps to connect two logical switches via a virtual port. Forwarding module may or may not support this functionality. 
* @ingroup management
*
* @param dpid_lsi1 Datapath ID of the LSI1
* @param dpid_lsi2 Datapath ID of the LSI2 
*/
hal_result_t hal_driver_connect_switches(uint64_t dpid_lsi1, switch_port_t** port1, uint64_t dpid_lsi2, switch_port_t** port2){

        ROFL_DEBUG("[AFA] driver_connect_switches (dpid_1: %d, dpid_2: %d)\n", dpid_lsi1, dpid_lsi2);
	of_switch_t *lsw1, *lsw2;

	//Check existance of the dpid
	lsw1 = physical_switch_get_logical_switch_by_dpid(dpid_lsi1);
	lsw2 = physical_switch_get_logical_switch_by_dpid(dpid_lsi2);

	if(!lsw1 || !lsw2){
		assert(0);
		return HAL_FAILURE;
	}
	

	return HAL_SUCCESS; 
}

/*
* @name    hal_driver_detach_port_from_switch
* @brief   Detaches a port from the switch 
* @ingroup port_management
*
* @param dpid Datapath ID of the switch to detach the ports
* @param name Port name (system's name)
*/
hal_result_t hal_driver_detach_port_from_switch(uint64_t dpid, const char* name){

        ROFL_DEBUG("[AFA] driver_detach_port_from_switch (dpid: %d, name: %s)\n", dpid, name);
	of_switch_t* lsw;
	switch_port_t* port;
    switch_port_snapshot_t* port_snapshot;
	
	lsw = physical_switch_get_logical_switch_by_dpid(dpid);
	if(!lsw)
		return HAL_FAILURE;

	port = physical_switch_get_port_by_name(name);

	//Check if the port does exist and is really attached to the dpid
	if( !port || port->attached_sw->dpid != dpid)
		return HAL_FAILURE;

	if(physical_switch_detach_port_from_logical_switch(port,lsw) != ROFL_SUCCESS)
		return HAL_FAILURE;
	
	//notify port dettached
    port_snapshot = physical_switch_get_port_snapshot(port->name);
	if(hal_cmm_notify_port_delete(port_snapshot) != HAL_SUCCESS){
		///return HAL_FAILURE; //ignore
	}

	//If it is virtual remove also the data structures associated
	if(port->type == PORT_TYPE_VIRTUAL){
		//Remove from the pipeline and delete
		if(physical_switch_remove_port(port->name) != ROFL_SUCCESS){
			ROFL_ERR("Error removing port from the physical_switch. The port may become unusable...\n");
			assert(0);
			return HAL_FAILURE;
			
		}
	}
	
	return HAL_SUCCESS; 
}


/*
* @name    hal_driver_detach_port_from_switch_at_port_num
* @brief   Detaches port_num of the logical switch identified with dpid 
* @ingroup port_management
*
* @param dpid Datapath ID of the switch to detach the ports
* @param of_port_num Number of the port (OF number) 
*/
hal_result_t hal_driver_detach_port_from_switch_at_port_num(uint64_t dpid, const unsigned int of_port_num){

        ROFL_DEBUG("[AFA] driver_detach_port_from_switch_at_port_num (dpid: %d, port: %d)\n", dpid, of_port_num);
	of_switch_t* lsw;
	
	lsw = physical_switch_get_logical_switch_by_dpid(dpid);
	if(!lsw)
		return HAL_FAILURE;

	//Check if the port does exist.
	if(!of_port_num || of_port_num >= LOGICAL_SWITCH_MAX_LOG_PORTS || !lsw->logical_ports[of_port_num].port)
		return HAL_FAILURE;

	return hal_driver_detach_port_from_switch(dpid, lsw->logical_ports[of_port_num].port->name);
}


//Port admin up/down stuff

/*
* Port administrative management actions (ifconfig up/down like)
*/

/*
* @name    hal_driver_enable_port
* @brief   Brings up a system port. If the port is attached to an OF logical switch, this also schedules port for I/O and triggers PORTMOD message. 
* @ingroup port_management
*
* @param name Port system name 
*/
hal_result_t hal_driver_bring_port_up(const char* name){
    
        ROFL_DEBUG("[AFA] hal_driver_bring_port_up (name: %s)\n", name);

	switch_port_t* port;
	switch_port_snapshot_t* port_snapshot;

	//Check if the port does exist
	port = physical_switch_get_port_by_name(name);

	if(!port)
		return HAL_FAILURE;
        
        // Assing channel for packet exchange from/to EZ
        port->platform_port_state = (platform_port_state_t*)get_ez_packet_channel();

	port_snapshot = physical_switch_get_port_snapshot(port->name);
    hal_cmm_notify_port_status_changed(port_snapshot);
	
	return HAL_SUCCESS;
}

/*
* @name    hal_driver_disable_port
* @brief   Shutdowns (brings down) a system port. If the port is attached to an OF logical switch, this also de-schedules port and triggers PORTMOD message. 
* @ingroup port_management
*
* @param name Port system name 
*/
hal_result_t hal_driver_bring_port_down(const char* name){

        ROFL_DEBUG("[AFA] hal_driver_bring_port_down (name: %s)\n", name);
        
	switch_port_t* port;
	switch_port_snapshot_t* port_snapshot;
	
	//Check if the port does exist
	port = physical_switch_get_port_by_name(name);
        
	//Check if the port does exist
	if(!port || !port->platform_port_state)
		return HAL_FAILURE;
        
        port->platform_port_state = NULL;

    port_snapshot = physical_switch_get_port_snapshot(port->name); 
	if(hal_cmm_notify_port_status_changed(port_snapshot) != HAL_SUCCESS)
		return HAL_FAILURE;
	
	return HAL_SUCCESS;
}

/*
* @name    hal_driver_enable_port_by_num
* @brief   Brings up a port from an OF logical switch (and the underlying physical interface). This function also triggers the PORTMOD message 
* @ingroup port_management
*
* @param dpid DatapathID 
* @param port_num OF port number
*/
hal_result_t hal_driver_bring_port_up_by_num(uint64_t dpid, unsigned int port_num){

        ROFL_DEBUG("[AFA] hal_driver_bring_port_up_by_num (dpid: %d, port: %d\n", dpid, port_num);
	of_switch_t* lsw;
	switch_port_snapshot_t* port_snapshot;
	
	lsw = physical_switch_get_logical_switch_by_dpid(dpid);
	if(!lsw)
		return HAL_FAILURE;

	//Check if the port does exist and is really attached to the dpid
	if( !lsw->logical_ports[port_num].port || lsw->logical_ports[port_num].attachment_state != LOGICAL_PORT_STATE_ATTACHED || lsw->logical_ports[port_num].port->attached_sw->dpid != dpid)
		return HAL_FAILURE;

        lsw->logical_ports[port_num].port->platform_port_state = (platform_port_state_t*)get_ez_packet_channel();
	
	port_snapshot = physical_switch_get_port_snapshot(lsw->logical_ports[port_num].port->name); 
    hal_cmm_notify_port_status_changed(port_snapshot);
	
	return HAL_SUCCESS;
}

/*
* @name    hal_driver_disable_port_by_num
* @brief   Brings down a port from an OF logical switch (and the underlying physical interface). This also triggers the PORTMOD message.
* @ingroup port_management
*
* @param dpid DatapathID 
* @param port_num OF port number
*/
hal_result_t hal_driver_bring_port_down_by_num(uint64_t dpid, unsigned int port_num){

        ROFL_DEBUG("[AFA] hal_driver_bring_port_down_by_num (dpid: %d, port: %d\n", dpid, port_num);
	of_switch_t* lsw;
	switch_port_snapshot_t* port_snapshot;
	
	lsw = physical_switch_get_logical_switch_by_dpid(dpid);
	if(!lsw)
		return HAL_FAILURE;

	//Check if the port does exist and is really attached to the dpid
	if( !lsw->logical_ports[port_num].port || lsw->logical_ports[port_num].attachment_state != LOGICAL_PORT_STATE_ATTACHED || lsw->logical_ports[port_num].port->attached_sw->dpid != dpid)
		return HAL_FAILURE;

        lsw->logical_ports[port_num].port->platform_port_state = NULL;
	
    port_snapshot = physical_switch_get_port_snapshot(lsw->logical_ports[port_num].port->name); 
    hal_cmm_notify_port_status_changed(port_snapshot);
	
	return HAL_SUCCESS;
}

/**
 * @brief Retrieve a snapshot of the monitoring state. If rev is 0, or the current monitoring 
 * has changed (monitoring->rev != rev), a new snapshot of the monitoring state is made. Warning: this 
 * is expensive.
 * @ingroup hal_driver_management
 *
 * @param rev Last seen revision. Set to 0 to always get a new snapshot 
 * @return A snapshot of the monitoring state that MUST be destroyed using monitoring_destroy_snapshot() or NULL if there have been no changes (same rev)
 */ 
monitoring_snapshot_state_t* hal_driver_get_monitoring_snapshot(uint64_t rev){

	monitoring_state_t* mon = physical_switch_get_monitoring();

	if( rev == 0 || monitoring_has_changed(mon, &rev) ) 
		return monitoring_get_snapshot(mon);

	return NULL;
}

/**
 * @brief get a list of available matching algorithms
 * @ingroup driver_management
 *
 * @param of_version
 * @param name_list
 * @param count
 * @return
 */
hal_result_t hal_driver_list_matching_algorithms(of_version_t of_version, const char * const** name_list, int *count){
        ROFL_DEBUG("[AFA] driver_list_matching_algorithms\n");
	return (hal_result_t)of_get_switch_matching_algorithms(of_version, name_list, count);
}
