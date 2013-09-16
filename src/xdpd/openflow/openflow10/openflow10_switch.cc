/*
 * openflow10_switch.cc
 *
 *  Created on: 06.09.2013
 *      Author: andreas
 */

#include "openflow10_switch.h"

#include <rofl/platform/unix/csyslog.h>
#include <rofl/datapath/afa/openflow/openflow1x/of1x_fwd_module.h>
#include <rofl/datapath/afa/openflow/openflow1x/of1x_cmm.h>

using namespace rofl;

/*
* Constructor and destructor for the openflow 1.0 switch
*/
openflow10_switch::openflow10_switch(uint64_t dpid,
				std::string const& dpname,
				unsigned int num_of_tables,
				int* ma_list,
				caddress const& controller_addr,
				caddress const& binding_addr) throw (eOfSmVersionNotSupported)
		: openflow_switch(dpid, dpname, version)
{

	if ((ofswitch = fwd_module_create_switch((char*)dpname.c_str(),
					     dpid, OF_VERSION_10, num_of_tables, ma_list)) == NULL){
		//WRITELOG(CDATAPATH, ERROR, "of10_endpoint::of10_endpoint() "
		//		"failed to allocate switch instance in HAL, aborting");

		throw eOfSmErrorOnCreation();
	}

	//Initialize the endpoint, and launch control channel
	endpoint = new of10_endpoint(this, controller_addr, binding_addr);

}


openflow10_switch::~openflow10_switch(){

	//Destroy listening sockets and ofctl instances
	endpoint->rpc_close_all();

	//Now safely destroy the endpoint
	delete endpoint;

	//Destroy forwarding plane state
	fwd_module_destroy_switch_by_dpid(dpid);
}

/* Public interfaces for receving async messages from the driver */
afa_result_t openflow10_switch::process_packet_in(uint8_t table_id,
					uint8_t reason,
					uint32_t in_port,
					uint32_t buffer_id,
					uint8_t* pkt_buffer,
					uint32_t buf_len,
					uint16_t total_len,
					of1x_packet_matches_t matches){

	return ((of10_endpoint*)endpoint)->process_packet_in(table_id,
					reason,
					in_port,
					buffer_id,
					pkt_buffer,
					buf_len,
					total_len,
					matches);
}

afa_result_t openflow10_switch::process_flow_removed(uint8_t reason, of1x_flow_entry_t* removed_flow_entry){
	return ((of10_endpoint*)endpoint)->process_flow_removed(reason, removed_flow_entry);
}

/*
* Port notfications. Process them directly in the endpoint
*/
afa_result_t openflow10_switch::notify_port_add(switch_port_t* port){
	return endpoint->notify_port_add(port);
}
afa_result_t openflow10_switch::notify_port_delete(switch_port_t* port){
	return endpoint->notify_port_delete(port);
}
afa_result_t openflow10_switch::notify_port_status_changed(switch_port_t* port){
	return endpoint->notify_port_status_changed(port);
}



/*
* Driver HAL calls. Demultiplexing to the appropiate openflow10_switch instance.
*/

afa_result_t cmm_process_of10_packet_in(const of1x_switch_t* sw,
					uint8_t table_id,
					uint8_t reason,
					uint32_t in_port,
					uint32_t buffer_id,
					uint8_t* pkt_buffer,
					uint32_t buf_len,
					uint16_t total_len,
					of1x_packet_matches_t matches)
{
	openflow10_switch* dp=NULL;

	if (!sw)
		return AFA_FAILURE;

	try{
		dp = dynamic_cast<openflow10_switch*> (switch_manager::find_by_dpid(sw->dpid));
        }catch (const std::bad_cast& e){
		return AFA_FAILURE;
        }

	if(!dp)
		return AFA_FAILURE;

	return dp->process_packet_in(table_id,
					reason,
					in_port,
					buffer_id,
					pkt_buffer,
					buf_len,
					total_len,
					matches);
}

afa_result_t cmm_process_of10_flow_removed(const of1x_switch_t* sw, uint8_t reason, of1x_flow_entry_t* removed_flow_entry){

	openflow10_switch* dp=NULL;

	if (!sw)
		return AFA_FAILURE;

	try{
		dp = dynamic_cast<openflow10_switch*> (switch_manager::find_by_dpid(sw->dpid));
        }catch (const std::bad_cast& e){
		return AFA_FAILURE;
        }

	if(!dp)
		return AFA_FAILURE;

	return dp->process_flow_removed(reason, removed_flow_entry);
}



