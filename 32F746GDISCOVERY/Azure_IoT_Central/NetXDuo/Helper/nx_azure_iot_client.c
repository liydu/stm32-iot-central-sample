/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_azure_iot.c
  * @author  Microsoft
  * @brief   Azure IoT application file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2022 Microsoft.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "nx_azure_iot_client.h"

#include "nx_azure_iot_cert.h"
#include "nx_azure_iot_ciphersuites.h"

#include "nx_azure_iot_connect.h"

#include "nx_azure_iot_hub_client_properties.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* nx_azure_iot_create priority */
#define NX_AZURE_IOT_THREAD_PRIORITY 4

/* Incoming events from the middleware. */
#define HUB_ALL_EVENTS                        0xFF
#define HUB_CONNECT_EVENT                     0x01
#define HUB_DISCONNECT_EVENT                  0x02
#define HUB_COMMAND_RECEIVE_EVENT             0x04
#define HUB_PROPERTIES_RECEIVE_EVENT          0x08
#define HUB_WRITABLE_PROPERTIES_RECEIVE_EVENT 0x10
#define HUB_PROPERTIES_COMPLETE_EVENT         0x20
#define HUB_PERIODIC_TIMER_EVENT              0x40

#define DPS_ENDPOINT "global.azure-devices-provisioning.net"
#define DPS_PAYLOAD  "{\"modelId\":\"%s\"}"

#define MODULE_ID ""

/* Connection timeouts in threadx ticks. */
#define HUB_CONNECT_TIMEOUT_TICKS  (10 * TX_TIMER_TICKS_PER_SECOND)
#define DPS_REGISTER_TIMEOUT_TICKS (30 * TX_TIMER_TICKS_PER_SECOND)

#define DPS_PAYLOAD_SIZE       (15 + 128)
#define TELEMETRY_BUFFER_SIZE  256
#define PROPERTIES_BUFFER_SIZE 128

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
static UCHAR telemetry_buffer[TELEMETRY_BUFFER_SIZE];
static UCHAR properties_buffer[PROPERTIES_BUFFER_SIZE];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* USER CODE BEGIN 1 */

/* Helper */
static VOID printf_packet(CHAR* prepend, NX_PACKET* packet_ptr)
{
  printf("%s", prepend);

  while (packet_ptr != NX_NULL)
  {
    printf("%.*s", (INT)(packet_ptr->nx_packet_length), (CHAR*)packet_ptr->nx_packet_prepend_ptr);
    packet_ptr = packet_ptr->nx_packet_next;
  }

  printf("\r\n");
}

static VOID connection_status_callback(NX_AZURE_IOT_HUB_CLIENT* hub_client_ptr, UINT status)
{
  // :HACK: This callback doesn't allow us to provide context, pinch it from the command message callback args
  AZURE_IOT_CONTEXT* nx_context = hub_client_ptr->nx_azure_iot_hub_client_command_message.message_callback_args;

  if (status == NX_SUCCESS)
  {
    tx_event_flags_set(&nx_context->events, HUB_CONNECT_EVENT, TX_OR);
  }
  else
  {
    tx_event_flags_set(&nx_context->events, HUB_DISCONNECT_EVENT, TX_OR);
  }

  /* Update the connection status in the connect workflow. */
  connection_status_set(nx_context, status);
}

static VOID message_receive_command(NX_AZURE_IOT_HUB_CLIENT* hub_client_ptr, VOID* context)
{
  AZURE_IOT_CONTEXT* nx_context = (AZURE_IOT_CONTEXT*)context;
  tx_event_flags_set(&nx_context->events, HUB_COMMAND_RECEIVE_EVENT, TX_OR);
}

static VOID message_receive_callback_properties(NX_AZURE_IOT_HUB_CLIENT* hub_client_ptr, VOID* context)
{
  AZURE_IOT_CONTEXT* nx_context = (AZURE_IOT_CONTEXT*)context;
  tx_event_flags_set(&nx_context->events, HUB_PROPERTIES_RECEIVE_EVENT, TX_OR);
}

static VOID message_receive_callback_writable_property(NX_AZURE_IOT_HUB_CLIENT* hub_client_ptr, VOID* context)
{
  AZURE_IOT_CONTEXT* nx_context = (AZURE_IOT_CONTEXT*)context;
  tx_event_flags_set(&nx_context->events, HUB_WRITABLE_PROPERTIES_RECEIVE_EVENT, TX_OR);
}

static VOID periodic_timer_entry(ULONG context)
{
  AZURE_IOT_CONTEXT* nx_context = (AZURE_IOT_CONTEXT*)context;
  tx_event_flags_set(&nx_context->events, HUB_PERIODIC_TIMER_EVENT, TX_OR);
}

/**
 * @brief  Initialize IoT Hub client.
 * @param context: AZURE_IOT_CONTEXT
 * @retval status
 */
static UINT iot_hub_initialize(AZURE_IOT_CONTEXT* context)
{
  UINT status;

  /* Initialize IoT Hub client. */
  if ((status = nx_azure_iot_hub_client_initialize(&context->iothub_client,
           &context->nx_azure_iot,
           (UCHAR*)context->azure_iot_hub_hostname,
           context->azure_iot_hub_hostname_length,
           (UCHAR*)context->azure_iot_hub_device_id,
           context->azure_iot_hub_device_id_length,
           (UCHAR*)MODULE_ID,
           sizeof(MODULE_ID) - 1,
           _nx_azure_iot_tls_supported_crypto,
           _nx_azure_iot_tls_supported_crypto_size,
           _nx_azure_iot_tls_ciphersuite_map,
           _nx_azure_iot_tls_ciphersuite_map_size,
           (UCHAR*)context->nx_azure_iot_tls_metadata_buffer,
           sizeof(context->nx_azure_iot_tls_metadata_buffer),
           &context->root_ca_cert)))
  {
    printf("Error: on nx_azure_iot_hub_client_initialize (0x%08x)\r\n", status);
    return status;
  }

  /* Set credentials. */
  if (context->azure_iot_auth_mode == AZURE_IOT_AUTH_MODE_SAS)
  {
    /* Symmetric (SAS) Key. */
    if ((status = nx_azure_iot_hub_client_symmetric_key_set(&context->iothub_client,
             (UCHAR*)context->azure_iot_device_sas_key,
             context->azure_iot_device_sas_key_length)))
    {
      printf("Error: failed on nx_azure_iot_hub_client_symmetric_key_set (0x%08x)\r\n", status);
    }
  }
  else if (context->azure_iot_auth_mode == AZURE_IOT_AUTH_MODE_CERT)
  {
    /* X509 Certificate. */
    if ((status = nx_azure_iot_hub_client_device_cert_set(&context->iothub_client, &context->device_certificate)))
    {
      printf("Error: failed on nx_azure_iot_hub_client_device_cert_set!: error code = 0x%08x\r\n", status);
    }
  }

  if (status != NX_AZURE_IOT_SUCCESS)
  {
    printf("Failed to set auth credentials\r\n");
  }

  // Add more CA certificates
  else if ((status = nx_azure_iot_hub_client_trusted_cert_add(&context->iothub_client, &context->root_ca_cert_2)))
  {
    printf("Failed on nx_azure_iot_hub_client_trusted_cert_add!: error code = 0x%08x\r\n", status);
  }
  else if ((status = nx_azure_iot_hub_client_trusted_cert_add(&context->iothub_client, &context->root_ca_cert_3)))
  {
    printf("Failed on nx_azure_iot_hub_client_trusted_cert_add!: error code = 0x%08x\r\n", status);
  }

  /* Set Model id. */
  else if ((status = nx_azure_iot_hub_client_model_id_set(&context->iothub_client,
                (UCHAR*)context->azure_iot_model_id,
                context->azure_iot_model_id_length)))
  {
    printf("Error: nx_azure_iot_hub_client_model_id_set (0x%08x)\r\n", status);
  }

  /* Set connection status callback. */
  else if ((status = nx_azure_iot_hub_client_connection_status_callback_set(
                &context->iothub_client, connection_status_callback)))
  {
    printf("Error: failed on connection_status_callback (0x%08x)\r\n", status);
  }

  /* Enable commands. */
  else if ((status = nx_azure_iot_hub_client_command_enable(&context->iothub_client)))
  {
    printf("Error: command receive enable failed (0x%08x)\r\n", status);
  }

  /* Enable properties. */
  else if ((status = nx_azure_iot_hub_client_properties_enable(&context->iothub_client)))
  {
    printf("Failed on nx_azure_iot_hub_client_properties_enable!: error code = 0x%08x\r\n", status);
  }

  /* Set properties callback. */
  else if ((status = nx_azure_iot_hub_client_receive_callback_set(&context->iothub_client,
                NX_AZURE_IOT_HUB_PROPERTIES,
                message_receive_callback_properties,
                (VOID*)context)))
  {
    printf("Error: device twin callback set (0x%08x)\r\n", status);
  }

  /* Set command callback. */
  else if ((status = nx_azure_iot_hub_client_receive_callback_set(
                &context->iothub_client, NX_AZURE_IOT_HUB_COMMAND, message_receive_command, (VOID*)context)))
  {
    printf("Error: device method callback set (0x%08x)\r\n", status);
  }

  /* Set the writable property callback. */
  else if ((status = nx_azure_iot_hub_client_receive_callback_set(&context->iothub_client,
                NX_AZURE_IOT_HUB_WRITABLE_PROPERTIES,
                message_receive_callback_writable_property,
                (VOID*)context)))
  {
    printf("Error: device twin desired property callback set (0x%08x)\r\n", status);
  }

  if (status != NX_AZURE_IOT_SUCCESS)
  {
    nx_azure_iot_hub_client_deinitialize(&context->iothub_client);
  }

  return status;
}

/**
 * @brief  Initialize DPS client.
 * @param context: AZURE_IOT_CONTEXT
 * @retval iot_hub_initialize() status
 */
static UINT dps_initialize(AZURE_IOT_CONTEXT* context)
{
  UINT status;
  CHAR payload[DPS_PAYLOAD_SIZE];

  if (context == NULL)
  {
    printf("ERROR: context is NULL\r\n");
    return NX_PTR_ERROR;
  }

  // Return error if empty credentials
  if (context->azure_iot_dps_id_scope_length == 0 || context->azure_iot_dps_registration_id_length == 0)
  {
    printf("ERROR: azure_iot_nx_client_dps_entry incorrect parameters\r\n");
    return NX_PTR_ERROR;
  }

  printf("\r\nInitializing Azure IoT DPS client\r\n");
  printf("\tDPS endpoint: %s\r\n", DPS_ENDPOINT);
  printf("\tDPS ID scope: %.*s\r\n", context->azure_iot_dps_id_scope_length, context->azure_iot_dps_id_scope);
  printf("\tRegistration ID: %.*s\r\n",
      context->azure_iot_dps_registration_id_length,
      context->azure_iot_dps_registration_id);

  // Initialise the length of the return buffers
  context->azure_iot_hub_hostname_length  = sizeof(context->azure_iot_hub_hostname);
  context->azure_iot_hub_device_id_length = sizeof(context->azure_iot_hub_device_id);

  if (snprintf(payload, sizeof(payload), DPS_PAYLOAD, context->azure_iot_model_id) > DPS_PAYLOAD_SIZE - 1)
  {
    printf("ERROR: insufficient buffer size to create DPS payload\r\n");
    return NX_SIZE_ERROR;
  }

  // Initialize IoT provisioning client
  if ((status = nx_azure_iot_provisioning_client_initialize(&context->dps_client,
           &context->nx_azure_iot,
           (UCHAR*)DPS_ENDPOINT,
           strlen(DPS_ENDPOINT),
           (UCHAR*)context->azure_iot_dps_id_scope,
           context->azure_iot_dps_id_scope_length,
           (UCHAR*)context->azure_iot_dps_registration_id,
           context->azure_iot_dps_registration_id_length,
           _nx_azure_iot_tls_supported_crypto,
           _nx_azure_iot_tls_supported_crypto_size,
           _nx_azure_iot_tls_ciphersuite_map,
           _nx_azure_iot_tls_ciphersuite_map_size,
           (UCHAR*)context->nx_azure_iot_tls_metadata_buffer,
           sizeof(context->nx_azure_iot_tls_metadata_buffer),
           &context->root_ca_cert)))
  {
    printf("ERROR: nx_azure_iot_provisioning_client_initialize (0x%08x)\r\n", status);
    return status;
  }

  // Add more CA certificates
  else if ((status = nx_azure_iot_provisioning_client_trusted_cert_add(
                &context->dps_client, &context->root_ca_cert_2)))
  {
    printf("ERROR: nx_azure_iot_provisioning_client_trusted_cert_add!: error code = 0x%08x\r\n", status);
  }
  else if ((status = nx_azure_iot_provisioning_client_trusted_cert_add(
                &context->dps_client, &context->root_ca_cert_3)))
  {
    printf("ERROR: nx_azure_iot_provisioning_client_trusted_cert_add!: error code = 0x%08x\r\n", status);
  }

  else
  {
    switch (context->azure_iot_auth_mode)
    {
      // Symmetric (SAS) Key
      case AZURE_IOT_AUTH_MODE_SAS:
        if ((status = nx_azure_iot_provisioning_client_symmetric_key_set(&context->dps_client,
                 (UCHAR*)context->azure_iot_device_sas_key,
                 context->azure_iot_device_sas_key_length)))
        {
          printf("ERROR: nx_azure_iot_provisioning_client_symmetric_key_set (0x%08x)\r\n", status);
        }
        break;

      // X509 Certificate
      case AZURE_IOT_AUTH_MODE_CERT:
        if ((status = nx_azure_iot_provisioning_client_device_cert_set(
                 &context->dps_client, &context->device_certificate)))
        {
          printf("ERROR: nx_azure_iot_provisioning_client_device_cert_set (0x%08x)\r\n", status);
        }
        break;
    }
  }

  if (status != NX_AZURE_IOT_SUCCESS)
  {
    printf("ERROR: failed to set initialize DPS\r\n");
  }

  // Set the payload containing the model Id
  else if ((status = nx_azure_iot_provisioning_client_registration_payload_set(
                &context->dps_client, (UCHAR*)payload, strlen(payload))))
  {
    printf("ERROR: nx_azure_iot_provisioning_client_registration_payload_set (0x%08x\r\n", status);
  }

  else if ((status = nx_azure_iot_provisioning_client_register(&context->dps_client, DPS_REGISTER_TIMEOUT_TICKS)))
  {
    printf("\tERROR: nx_azure_iot_provisioning_client_register (0x%08x)\r\n", status);
  }

  // Stash IoT Hub Device info
  else if ((status = nx_azure_iot_provisioning_client_iothub_device_info_get(&context->dps_client,
                (UCHAR*)context->azure_iot_hub_hostname,
                &context->azure_iot_hub_hostname_length,
                (UCHAR*)context->azure_iot_hub_device_id,
                &context->azure_iot_hub_device_id_length)))
  {
    printf("ERROR: nx_azure_iot_provisioning_client_iothub_device_info_get (0x%08x)\r\n", status);
  }

  // Destroy Provisioning Client
  nx_azure_iot_provisioning_client_deinitialize(&context->dps_client);

  if (status != NX_SUCCESS)
  {
    return status;
  }

  printf("SUCCESS: Azure IoT DPS client initialized\r\n");

  return iot_hub_initialize(context);
}

static VOID process_connect(AZURE_IOT_CONTEXT* context)
{
  UINT status;

  // Request the client properties
  if ((status = nx_azure_iot_hub_client_properties_request(&context->iothub_client, NX_WAIT_FOREVER)))
  {
    printf("ERROR: failed to request properties (0x%08x)\r\n", status);
  }

  // Start the periodic timer
  if ((status = tx_timer_activate(&context->periodic_timer)))
  {
    printf("ERROR: tx_timer_activate (0x%08x)\r\n", status);
  }
}

static VOID process_disconnect(AZURE_IOT_CONTEXT* context)
{
  UINT status;

  printf("Disconnected from IoT Hub\r\n");

  // Stop the periodic timer
  if ((status = tx_timer_deactivate(&context->periodic_timer)))
  {
    printf("ERROR: tx_timer_deactivate (0x%08x)\r\n", status);
  }
}

static VOID process_timer_event(AZURE_IOT_CONTEXT* context)
{
  if (context->timer_cb)
  {
    context->timer_cb(context);
  }
}

UINT nx_azure_iot_client_publish_telemetry(
  AZURE_IOT_CONTEXT* context, CHAR* component_name_ptr, 
  UINT (*append_properties)(NX_AZURE_IOT_JSON_WRITER* json_writer_ptr))
{
  UINT status;
  UINT telemetry_length;
  NX_PACKET* packet_ptr;
  NX_AZURE_IOT_JSON_WRITER json_writer;

  if ((status = nx_azure_iot_hub_client_telemetry_message_create(
           &context->iothub_client, &packet_ptr, NX_WAIT_FOREVER)))
  {
    printf("Error: nx_azure_iot_hub_client_telemetry_message_create failed (0x%08x)\r\n", status);
  }

  if ((status = nx_azure_iot_json_writer_with_buffer_init(&json_writer, telemetry_buffer, sizeof(telemetry_buffer))))
  {
    printf("Error: Failed to initialize json writer (0x%08x)\r\n", status);
    nx_azure_iot_hub_client_telemetry_message_delete(packet_ptr);
    return status;
  }

  if ((status = nx_azure_iot_json_writer_append_begin_object(&json_writer)) ||
      (component_name_ptr != NX_NULL &&
          (status = nx_azure_iot_hub_client_reported_properties_component_begin(
               &context->iothub_client, &json_writer, (UCHAR*)component_name_ptr, strlen(component_name_ptr)))) ||
      (status = append_properties(&json_writer)) ||
      (component_name_ptr != NX_NULL && (status = nx_azure_iot_hub_client_reported_properties_component_end(
                                             &context->iothub_client, &json_writer))) ||
      (status = nx_azure_iot_json_writer_append_end_object(&json_writer)))
  {
    printf("Error: Failed to build telemetry (0x%08x)\r\n", status);
    nx_azure_iot_hub_client_telemetry_message_delete(packet_ptr);
    return status;
  }

  telemetry_length = nx_azure_iot_json_writer_get_bytes_used(&json_writer);
  if ((status = nx_azure_iot_hub_client_telemetry_send(
           &context->iothub_client, packet_ptr, telemetry_buffer, telemetry_length, NX_WAIT_FOREVER)))
  {
    printf("Error: Telemetry message send failed (0x%08x)\r\n", status);
    nx_azure_iot_hub_client_telemetry_message_delete(packet_ptr);
    return status;
  }

  printf("Telemetry message sent: %.*s.\r\n", telemetry_length, telemetry_buffer);

  return status;
}

UINT nx_azure_iot_client_periodic_interval_set(AZURE_IOT_CONTEXT* context, INT interval)
{
  UINT status;
  UINT active;
  UINT ticks = interval * TX_TIMER_TICKS_PER_SECOND;

  if ((status = tx_timer_info_get(&context->periodic_timer, NULL, &active, NULL, NULL, NULL)))
  {
    printf("ERROR: tx_timer_deactivate (0x%08x)\r\n", status);
    return status;
  }

  if (active == TX_TRUE && (status = tx_timer_deactivate(&context->periodic_timer)))
  {
    printf("ERROR: tx_timer_deactivate (0x%08x)\r\n", status);
  }

  else if ((status = tx_timer_change(&context->periodic_timer, ticks, ticks)))
  {
    printf("ERROR: tx_timer_change (0x%08x)\r\n", status);
  }

  else if (active == TX_TRUE && (status = tx_timer_activate(&context->periodic_timer)))
  {
    printf("ERROR: tx_timer_activate (0x%08x)\r\n", status);
  }

  return status;
}

static UINT reported_properties_begin(AZURE_IOT_CONTEXT* context_ptr,
    NX_AZURE_IOT_JSON_WRITER*                               json_writer,
    NX_PACKET**                                             packet_ptr,
    CHAR*                                                   component_name_ptr)
{
  UINT status;

  if ((status = nx_azure_iot_hub_client_reported_properties_create(
           &context_ptr->iothub_client, packet_ptr, NX_WAIT_FOREVER)))
  {
    printf("Error: Failed create reported properties (0x%08x)\r\n", status);
  }

  else if ((status = nx_azure_iot_json_writer_init(json_writer, *packet_ptr, NX_WAIT_FOREVER)))
  {
    printf("Error: Failed to initialize json writer (0x%08x)\r\n", status);
  }

  else if ((status = nx_azure_iot_json_writer_append_begin_object(json_writer)))
  {
    printf("Error: Failed to append object begin (0x%08x)\r\n", status);
  }

  else if (component_name_ptr != NX_NULL &&
           (status = nx_azure_iot_hub_client_reported_properties_component_begin(
                &context_ptr->iothub_client, json_writer, (UCHAR*)component_name_ptr, strlen(component_name_ptr))))
  {
    printf("Error: Failed to append component begin (0x%08x)\r\n", status);
  }

  return status;
}

static UINT reported_properties_end(
    AZURE_IOT_CONTEXT* context,
    NX_AZURE_IOT_JSON_WRITER*                             json_writer,
    NX_PACKET**                                           packet_ptr,
    CHAR*                                                 component_name_ptr)
{
  UINT status;
  UINT response_status = 0;

  if ((component_name_ptr != NX_NULL && (status = nx_azure_iot_hub_client_reported_properties_component_end(
                                             &context->iothub_client, json_writer))))
  {
    printf("Error: Failed to append component end (0x%08x)\r\n", status);
    return status;
  }

  if ((status = nx_azure_iot_json_writer_append_end_object(json_writer)))
  {
    printf("Error: Failed to append object end (0x%08x)\r\n", status);
    return status;
  }

  printf_packet("Sending property: ", *packet_ptr);

  if ((status = nx_azure_iot_hub_client_reported_properties_send(
           &context->iothub_client, *packet_ptr, NX_NULL, &response_status, NX_NULL, 5 * NX_IP_PERIODIC_RATE)))
  {
    printf("Error: nx_azure_iot_hub_client_reported_properties_send failed (0x%08x)\r\n", status);
    return status;
  }

  else if ((response_status < 200) || (response_status >= 300))
  {
    printf("Error: Property sent response status failed (%d)\r\n", response_status);
    return NX_NOT_SUCCESSFUL;
  }

  return NX_SUCCESS;
}

UINT nx_azure_iot_client_publish_properties(AZURE_IOT_CONTEXT* context,
    CHAR*                                                      component_name_ptr,
    UINT (*append_properties)(NX_AZURE_IOT_JSON_WRITER* json_writer_ptr))
{
  UINT                     status;
  NX_PACKET*               packet_ptr;
  NX_AZURE_IOT_JSON_WRITER json_writer;

  if ((status = reported_properties_begin(context, &json_writer, &packet_ptr, component_name_ptr)) ||

      (status = append_properties(&json_writer)) ||

      (status = reported_properties_end(context, &json_writer, &packet_ptr, component_name_ptr)))
  {
    printf("ERROR: azure_iot_nx_client_publish_properties (0x%08x)", status);
    nx_packet_release(packet_ptr);
  }

  return status;
}

UINT nx_azure_iot_client_publish_bool_property(
    AZURE_IOT_CONTEXT* context, CHAR* component_name_ptr, CHAR* property_ptr, bool value)
{
  UINT                     status;
  NX_AZURE_IOT_JSON_WRITER json_writer;
  NX_PACKET*               packet_ptr;

  if ((status = reported_properties_begin(context, &json_writer, &packet_ptr, component_name_ptr)) ||

      (status = nx_azure_iot_json_writer_append_property_with_bool_value(
           &json_writer, (const UCHAR*)property_ptr, strlen(property_ptr), value)) ||

      (status = reported_properties_end(context, &json_writer, &packet_ptr, component_name_ptr)))
  {
    printf("ERROR: azure_iot_nx_client_publish_bool_property (0x%08x)", status);
    nx_packet_release(packet_ptr);
  }

  return status;
}

UINT nx_azure_iot_client_register_properties_complete_callback(
    AZURE_IOT_CONTEXT* context, func_ptr_properties_complete callback)
{
  if (context == NULL || context->properties_complete_cb != NULL)
  {
    return NX_PTR_ERROR;
  }

  context->properties_complete_cb = callback;
  return NX_SUCCESS;
}

UINT nx_azure_iot_client_register_timer_callback(
    AZURE_IOT_CONTEXT* context, func_ptr_timer callback, int32_t interval)
{
  if (context == NULL || context->timer_cb != NULL)
  {
    return NX_PTR_ERROR;
  }

  nx_azure_iot_client_periodic_interval_set(context, interval);

  context->timer_cb = callback;

  return NX_SUCCESS;
}

UINT nx_azure_iot_client_sas_set(AZURE_IOT_CONTEXT* context, CHAR* device_sas_key)
{
  if (device_sas_key[0] == 0)
  {
    printf("Error: azure_iot_nx_client_sas_set device_sas_key is null\r\n");
    return NX_PTR_ERROR;
  }

  context->azure_iot_auth_mode             = AZURE_IOT_AUTH_MODE_SAS;
  context->azure_iot_device_sas_key        = device_sas_key;
  context->azure_iot_device_sas_key_length = strlen(device_sas_key);

  return NX_SUCCESS;
}

UINT nx_azure_iot_client_create(AZURE_IOT_CONTEXT* context,
  NX_IP*                                         nx_ip,
  NX_PACKET_POOL*                                nx_pool,
  NX_DNS*                                        nx_dns,
  UINT (*unix_time_callback)(ULONG* unix_time),
  CHAR* device_model_id,
  UINT  device_model_id_length)
{
  UINT ret = NX_SUCCESS;

  if (device_model_id_length == 0)
  {
    printf("ERROR: azure_iot_nx_client_create_new empty model_id\r\n");
    return NX_PTR_ERROR;
  }

  /* Initialise the context. */
  memset(context, 0, sizeof(AZURE_IOT_CONTEXT));

  /* Stash parameters. */
  context->azure_iot_connection_status = NX_AZURE_IOT_NOT_INITIALIZED;
  context->azure_iot_nx_ip             = nx_ip;
  context->azure_iot_model_id          = device_model_id;
  context->azure_iot_model_id_length   = device_model_id_length;

  /* Initialize CA root certificates. */
  ret = nx_secure_x509_certificate_initialize(&context->root_ca_cert,
      (UCHAR*)_nx_azure_iot_root_cert,
      (USHORT)_nx_azure_iot_root_cert_size,
      NX_NULL,
      0,
      NULL,
      0,
      NX_SECURE_X509_KEY_TYPE_NONE);

  if (ret != NX_SUCCESS)
  {
    printf("ERROR: nx_secure_x509_certificate_initialize (0x%08x)\r\n", ret);
    Error_Handler();
  }

  ret = nx_secure_x509_certificate_initialize(&context->root_ca_cert_2,
      (UCHAR*)_nx_azure_iot_root_cert_2,
      (USHORT)_nx_azure_iot_root_cert_size_2,
      NX_NULL,
      0,
      NULL,
      0,
      NX_SECURE_X509_KEY_TYPE_NONE);

  if (ret != NX_SUCCESS)
  {
    printf("ERROR: nx_secure_x509_certificate_initialize (0x%08x)\r\n", ret);
    Error_Handler();
  }

  ret = nx_secure_x509_certificate_initialize(&context->root_ca_cert_3,
      (UCHAR*)_nx_azure_iot_root_cert_3,
      (USHORT)_nx_azure_iot_root_cert_size_3,
      NX_NULL,
      0,
      NULL,
      0,
      NX_SECURE_X509_KEY_TYPE_NONE);

  if (ret != NX_SUCCESS)
  {
    printf("ERROR: nx_secure_x509_certificate_initialize (0x%08x)\r\n", ret);
    Error_Handler();
  }

  ret = tx_event_flags_create(&context->events, "nx_azure_iot_client");

  if (ret != NX_SUCCESS)
  {
    printf("ERROR: tx_event_flags_creates (0x%08x)\r\n", ret);
    Error_Handler();
  }

  ret = tx_timer_create(&context->periodic_timer,
      "periodic_timer",
      periodic_timer_entry,
      (ULONG)context,
      60 * NX_IP_PERIODIC_RATE,
      60 * NX_IP_PERIODIC_RATE,
      TX_NO_ACTIVATE);

  if (ret != NX_SUCCESS)
  {
    printf("ERROR: tx_timer_create (0x%08x)\r\n", ret);
    tx_event_flags_delete(&context->events);
    Error_Handler();
  }

  /* Create Azure IoT handler. */
  ret = nx_azure_iot_create(&context->nx_azure_iot,
      (UCHAR*)"Azure IoT",
      nx_ip,
      nx_pool,
      nx_dns,
      context->nx_azure_iot_thread_stack,
      sizeof(context->nx_azure_iot_thread_stack),
      NX_AZURE_IOT_THREAD_PRIORITY,
      unix_time_callback);

  if (ret != NX_SUCCESS)
  {
    printf("ERROR: failed on nx_azure_iot_create (0x%08x)\r\n", ret);
    tx_event_flags_delete(&context->events);
    tx_timer_delete(&context->periodic_timer);
    Error_Handler();
  }

  return ret;
 }

 static UINT client_run(
     AZURE_IOT_CONTEXT* context, UINT (*iot_initialize)(AZURE_IOT_CONTEXT*), UINT (*network_connect)())
 {
   ULONG app_events;

   while (true)
   {
     app_events = 0;
     tx_event_flags_get(&context->events, HUB_ALL_EVENTS, TX_OR_CLEAR, &app_events, NX_IP_PERIODIC_RATE);

     if (app_events & HUB_DISCONNECT_EVENT)
     {
       process_disconnect(context);
     }

     if (app_events & HUB_CONNECT_EVENT)
     {
       process_connect(context);
     }

     if (app_events & HUB_PERIODIC_TIMER_EVENT)
     {
       process_timer_event(context);
     }

     //if (app_events & HUB_PROPERTIES_COMPLETE_EVENT)
     //{
     //  process_properties_complete(context);
     //}

     //if (app_events & HUB_COMMAND_RECEIVE_EVENT)
     //{
     //  process_command(context);
     //}

     //if (app_events & HUB_PROPERTIES_RECEIVE_EVENT)
     //{
     //  process_properties(context);
     //}

     //if (app_events & HUB_WRITABLE_PROPERTIES_RECEIVE_EVENT)
     //{
     //  process_writable_properties(context);
     //}

     /* Mainain monitor and reconnect state */
     connection_monitor(context, iot_initialize, network_connect);
   }

   return NX_SUCCESS;
 }

 /**
  * @brief  IoT Hub client run.
  * @param context: AZURE_IOT_CONTEXT, 
  * @retval client_run() status
  */
 UINT nx_azure_iot_client_hub_run(
     AZURE_IOT_CONTEXT* context, CHAR* iot_hub_hostname, CHAR* iot_hub_device_id, UINT (*network_connect)())
 {
   if (iot_hub_hostname == 0 || iot_hub_device_id == 0)
   {
     printf("ERROR: azure_iot_nx_client_hub_run hub config is null\r\n");
     return NX_PTR_ERROR;
   }

   if (strlen(iot_hub_hostname) > AZURE_IOT_HOST_NAME_SIZE || strlen(iot_hub_device_id) > AZURE_IOT_DEVICE_ID_SIZE)
   {
     printf("ERROR: azure_iot_nx_client_hub_run hub config exceeds buffer size\r\n");
     return NX_SIZE_ERROR;
   }

   // take a copy of the hub config
   memcpy(context->azure_iot_hub_hostname, iot_hub_hostname, AZURE_IOT_HOST_NAME_SIZE);
   memcpy(context->azure_iot_hub_device_id, iot_hub_device_id, AZURE_IOT_DEVICE_ID_SIZE);
   context->azure_iot_hub_hostname_length  = strlen(iot_hub_hostname);
   context->azure_iot_hub_device_id_length = strlen(iot_hub_device_id);

   return client_run(context, iot_hub_initialize, network_connect);
 }

  /**
  * @brief  DPS client run.
  * @param context: AZURE_IOT_CONTEXT,
  * @retval client_run() status
  */
 UINT nx_azure_iot_client_dps_run(
     AZURE_IOT_CONTEXT* context, CHAR* dps_id_scope, CHAR* dps_registration_id, UINT (*network_connect)())
 {
   if (dps_id_scope == 0 || dps_registration_id == 0)
   {
     printf("ERROR: azure_iot_nx_client_dps_run dps config is null\r\n");
     return NX_PTR_ERROR;
   }

   // keep a reference to the dps config
   context->azure_iot_dps_id_scope               = dps_id_scope;
   context->azure_iot_dps_registration_id        = dps_registration_id;
   context->azure_iot_dps_id_scope_length        = strlen(dps_id_scope);
   context->azure_iot_dps_registration_id_length = strlen(dps_registration_id);

   return client_run(context, dps_initialize, network_connect);
 }


/* USER CODE END 1 */
