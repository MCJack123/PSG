#include <btstack.h>
#include <pico/cyw43_driver.h>
#include <pico/cyw43_arch.h>
#include <pico/btstack_cyw43.h>
#include <pico/btstack_run_loop_async_context.h>
#include <pico/async_context_poll.h>
#include <pico/btstack_hci_transport_cyw43.h>
#include "midi-btle.h"

struct MidiPacket {
    uint8_t usbcode;
    uint8_t command;
    uint8_t param1;
    uint8_t param2;
};

extern void handle_midi_packet(MidiPacket packet);

const uint8_t adv_data[] = {
    // Flags general discoverable
    0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    // Name
    0x09, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME, 'P', 'S', 'G', ' ', 'M', 'I', 'D', 'I', 
    // Incomplete List of 16-bit Service Class UUIDs -- FF10 - only valid for testing!
    0x11, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS, 0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03,
};
const uint8_t adv_data_len = sizeof(adv_data);

static btstack_packet_callback_registration_t hci_event_callback_registration;
static hci_con_handle_t con_handle;
static btstack_context_callback_registration_t  midi_callback;
static att_service_handler_t       midi_service;

static uint16_t midi_value_client_configuration;
static hci_con_handle_t midi_value_client_configuration_connection;

static uint16_t midi_value_handle;
static uint16_t midi_value_client_configuration_handle;

static uint8_t nul = 0;
static uint8_t initial[] = {0x80, 0x80};

static uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t * buffer, uint16_t buffer_size){
    UNUSED(connection_handle);

    if (att_handle == ATT_CHARACTERISTIC_7772E5DB_3868_4112_A1A9_F2669D106BF3_01_VALUE_HANDLE){
        return att_read_callback_handle_blob(initial, 2, offset, buffer, buffer_size);
    } else if (att_handle == ATT_CHARACTERISTIC_7772E5DB_3868_4112_A1A9_F2669D106BF3_01_CLIENT_CONFIGURATION_HANDLE) {
        return att_read_callback_handle_blob(&nul, 1, offset, buffer, buffer_size);
    }
    return 0;
}

static bool inSysEx = false;
static int att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size){
    UNUSED(transaction_mode);
    UNUSED(offset);
    UNUSED(buffer_size);
    
    if (att_handle != ATT_CHARACTERISTIC_7772E5DB_3868_4112_A1A9_F2669D106BF3_01_VALUE_HANDLE) return 0;
    int state = 0; // 0 = need timestamp, 1 = need status, 2 = need param1, 3 = need param2
    MidiPacket partial;
    for (int i = 1; i < buffer_size; i++) { // skip first byte (timestamp MSB)
        if (buffer[i] & 0x80) {
            switch (state) {
                case 0:
                    state = 1;
                    break;
                case 1:
                    partial.command = buffer[i];
                    if (buffer[i] == 0xF0) inSysEx = true;
                    state = 2;
                    break;
                case 2: case 3:
                    if (inSysEx) {
                        inSysEx = false;
                        if (partial.command == 0xF7) partial.usbcode = 0x05;
                        else if (partial.param1 == 0xF7) partial.usbcode = 0x06;
                        else if (partial.param2 == 0xF7) partial.usbcode = 0x07;
                        else {partial.usbcode = 0x04; inSysEx = true;}
                    } else partial.usbcode = partial.command >> 4;
                    handle_midi_packet(partial);
                    partial.param1 = partial.param2 = 0;
                    state = 1;
                    break;
            }
        } else {
            switch (state) {
                case 0: case 1: case 2:
                    partial.param1 = buffer[i];
                    state = 3;
                    break;
                case 3:
                    partial.param2 = buffer[i];
                    if (inSysEx) {
                        inSysEx = false;
                        if (partial.command == 0xF7) partial.usbcode = 0x05;
                        else if (partial.param1 == 0xF7) partial.usbcode = 0x06;
                        else if (partial.param2 == 0xF7) partial.usbcode = 0x07;
                        else {partial.usbcode = 0x04; inSysEx = true;}
                    } else partial.usbcode = partial.command >> 4;
                    handle_midi_packet(partial);
                    partial.param1 = partial.param2 = 0;
                    state = 0;
                    break;
            }
        }
    }
    if (state == 2 || state == 3) handle_midi_packet(partial);
    con_handle = connection_handle;
    return 0;
}

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            //le_notification_enabled = 0;
            break;
        case ATT_EVENT_CAN_SEND_NOW:
            //att_server_notify(con_handle, ATT_CHARACTERISTIC_7772E5DB_3868_4112_A1A9_F2669D106BF3_01_VALUE_HANDLE, (uint8_t*) counter_string, counter_string_len);
            break;
        default:
            break;
    }
}

bool le_midi_setup(void) {
    if (cyw43_arch_init()) return false;

    l2cap_init();

    // setup SM: Display only
    sm_init();

    // setup ATT server
    att_server_init(profile_data, att_read_callback, att_write_callback);    

	// get service handle range
	uint16_t start_handle = ATT_SERVICE_03B80E5A_EDE8_4B33_A751_6CE34EC4C700_START_HANDLE;
	uint16_t end_handle   = ATT_SERVICE_03B80E5A_EDE8_4B33_A751_6CE34EC4C700_END_HANDLE;

	// get characteristic value handle and client configuration handle
	midi_value_handle = ATT_CHARACTERISTIC_7772E5DB_3868_4112_A1A9_F2669D106BF3_01_VALUE_HANDLE;
	midi_value_client_configuration_handle = ATT_CHARACTERISTIC_7772E5DB_3868_4112_A1A9_F2669D106BF3_01_CLIENT_CONFIGURATION_HANDLE;

	// register service with ATT Server
	midi_service.start_handle   = start_handle;
	midi_service.end_handle     = end_handle;
	midi_service.read_callback  = &att_read_callback;
	midi_service.write_callback = &att_write_callback;
	att_server_register_service_handler(&midi_service);

    // setup advertisements
    uint16_t adv_int_min = 0x0030;
    uint16_t adv_int_max = 0x0030;
    uint8_t adv_type = 0;
    bd_addr_t null_addr;
    memset(null_addr, 0, 6);
    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(adv_data_len, (uint8_t*) adv_data);
    gap_advertisements_enable(1);

    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // register for ATT event
    att_server_register_packet_handler(packet_handler);

    hci_power_control(HCI_POWER_ON);
    return true;
}