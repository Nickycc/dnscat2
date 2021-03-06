/* driver_dns.c
 * Created July/2013
 * By Ron Bowes
 */
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "buffer.h"
#include "dns.h"
#include "log.h"
#include "memory.h"
#include "message.h"
#include "types.h"
#include "udp.h"

#include "driver_dns.h"

#define MAX_FIELD_LENGTH 62
#define MAX_DNS_LENGTH   255
#define WILDCARD_PREFIX  "dnscat"

/* The max length is a little complicated:
 * 255 because that's the max DNS length
 * Halved, because we encode in hex
 * Minus the length of the domain, which is appended
 * Minus 1, for the period right before the domain
 * Minus the number of periods that could appear within the name
 */
#define MAX_DNSCAT_LENGTH(domain) ((255/2) - (domain ? strlen(domain) : strlen(WILDCARD_PREFIX)) - 1 - ((MAX_DNS_LENGTH / MAX_FIELD_LENGTH) + 1))

static SELECT_RESPONSE_t dns_data_closed(void *group, int socket, void *param)
{
  LOG_FATAL("DNS socket closed!");
  exit(0);

  return SELECT_OK;
}

static uint8_t *remove_domain(char *str, char *domain)
{
  if(domain)
  {
    char *fixed = NULL;

    if(strlen(domain) > strlen(str))
    {
      LOG_ERROR("The string is too short to have a domain name attached: %s", str);
      return NULL;
    }

    fixed = safe_strdup(str);
    fixed[strlen(str) - strlen(domain) - 1] = '\0';

    return (uint8_t*)fixed;
  }
  else
  {
    return (uint8_t*)safe_strdup(str += strlen(WILDCARD_PREFIX));
  }
}

static uint8_t *buffer_decode_hex(uint8_t *str, size_t *length)
{
  size_t    i   = 0;
  buffer_t *out = buffer_create(BO_BIG_ENDIAN);

/*printf("Decoding: %s (%zu bytes)...\n", str, *length);*/
  while(i < *length)
  {
    uint8_t c1 = 0;
    uint8_t c2 = 0;

    /* Read the first character, ignoring periods */
    do
    {
      c1 = toupper(str[i++]);
    } while(c1 == '.' && i < *length);

    /* Make sure we aren't at the end of the buffer. */
    if(i >= *length)
    {
      LOG_ERROR("Couldn't hex-decode the name (name was an odd length): %s", str);
      return NULL;
    }

    /* Make sure we got a hex digit */
    if(!isxdigit(c1))
    {
      LOG_ERROR("Couldn't hex-decode the name (contains non-hex characters): %s", str);
      return NULL;
    }

    /* Read the second character. */
    do
    {
      c2 = toupper(str[i++]);
    } while(c2 == '.' && i < *length);

    /* Make sure we got a hex digit */
    if(!isxdigit(c2))
    {
      LOG_ERROR("Couldn't hex-decode the name (contains non-hex characters): %s", str);
      return NULL;
    }

    c1 = ((c1 < 'A') ? (c1 - '0') : (c1 - 'A' + 10));
    c2 = ((c2 < 'A') ? (c2 - '0') : (c2 - 'A' + 10));

    buffer_add_int8(out, (c1 << 4) | c2);
  }

  return buffer_create_string_and_destroy(out, length);
}

static SELECT_RESPONSE_t recv_socket_callback(void *group, int s, uint8_t *data, size_t length, char *addr, uint16_t port, void *param)
{
  /*driver_dns_t *driver_dns = param;*/
  dns_t        *dns    = dns_create_from_packet(data, length);
  driver_dns_t *driver = (driver_dns_t*) param;

  LOG_INFO("DNS response received (%d bytes)", length);

  /* TODO */
  if(dns->rcode != _DNS_RCODE_SUCCESS)
  {
    /* TODO: Handle errors more gracefully */
    switch(dns->rcode)
    {
      case _DNS_RCODE_FORMAT_ERROR:
        LOG_ERROR("DNS: RCODE_FORMAT_ERROR");
        break;
      case _DNS_RCODE_SERVER_FAILURE:
        LOG_ERROR("DNS: RCODE_SERVER_FAILURE");
        break;
      case _DNS_RCODE_NAME_ERROR:
        LOG_ERROR("DNS: RCODE_NAME_ERROR");
        break;
      case _DNS_RCODE_NOT_IMPLEMENTED:
        LOG_ERROR("DNS: RCODE_NOT_IMPLEMENTED");
        break;
      case _DNS_RCODE_REFUSED:
        LOG_ERROR("DNS: RCODE_REFUSED");
        break;
      default:
        LOG_ERROR("DNS: Unknown error code (0x%04x)", dns->rcode);
        break;
    }
  }
  else if(dns->question_count != 1)
  {
    LOG_ERROR("DNS returned the wrong number of response fields (question_count should be 1, was instead %d).", dns->question_count);
    LOG_ERROR("This is probably due to a DNS error");
  }
  else if(dns->answer_count < 1)
  {
    LOG_ERROR("DNS didn't return an answer");
    LOG_ERROR("This is probably due to a DNS error");
  }
  else
  {
    size_t    i;

    uint8_t   *answer = NULL;
    size_t     answer_length = 0;
    dns_type_t type = dns->answers[0].type;

    if(type == _DNS_TYPE_TEXT)
    {
      /* Get the answer. */
      answer        = dns->answers[0].answer->TEXT.text;
      answer_length = dns->answers[0].answer->TEXT.length;
      LOG_INFO("Received a TXT response (%zu bytes)", answer_length);

      /* Decode it. */
      answer = buffer_decode_hex(answer, &answer_length);
    }
    else if(type == _DNS_TYPE_CNAME)
    {
      /* Get the answer. */
      answer = remove_domain((char*)dns->answers[0].answer->CNAME.name, driver->domain);
      answer_length = strlen((char*)answer);
      LOG_INFO("Received a CNAME response (%zu bytes)", answer_length);

      /* Decode it. */
      answer = buffer_decode_hex(answer, &answer_length);
    }
    else if(type == _DNS_TYPE_MX)
    {
      /* Get the answer. */
      answer = remove_domain((char*)dns->answers[0].answer->MX.name, driver->domain);
      answer_length = strlen((char*)answer);
      LOG_INFO("Received a MX response (%zu bytes)", answer_length);

      /* Decode it. */
      answer = buffer_decode_hex(answer, &answer_length);
    }
    else if(type == _DNS_TYPE_A)
    {
      buffer_t *buf = buffer_create(BO_BIG_ENDIAN);

      for(i = 0; i < dns->answer_count; i++)
        buffer_add_bytes(buf, dns->answers[i].answer->A.bytes, 4);

      answer_length = buffer_read_next_int8(buf);
      LOG_INFO("Received an A response (%zu bytes)", answer_length);

      answer = safe_malloc(answer_length);
      buffer_read_bytes_at(buf, 1, answer, answer_length);
    }
#ifndef WIN32
    else if(type == _DNS_TYPE_AAAA)
    {
      buffer_t *buf = buffer_create(BO_BIG_ENDIAN);

      for(i = 0; i < dns->answer_count; i++)
        buffer_add_bytes(buf, dns->answers[i].answer->AAAA.bytes, 16);

      answer_length = buffer_read_next_int8(buf);
      LOG_INFO("Received an AAAA response (%zu bytes)", answer_length);

      answer = safe_malloc(answer_length);
      buffer_read_bytes_at(buf, 1, answer, answer_length);
    }
#endif
    else
    {
      LOG_ERROR("Unknown DNS type returned: %d", type);
      answer = NULL;
    }

    if(answer)
    {
      /*LOG_WARNING("Received a %zu-byte DNS response: %s [0x%04x]", answer_length, answer, type);*/

      /* Pass the buffer to the caller */
      if(answer_length > 0)
      {
        /* Pass the data elsewhere. */
        message_post_packet_in(answer, answer_length);
      }

      safe_free(answer);
    }
  }

  dns_destroy(dns);

  return SELECT_OK;
}

/* This function expects to receive the proper length of data. */
static void handle_packet_out(driver_dns_t *driver, uint8_t *data, size_t length)
{
  size_t        i;
  dns_t        *dns;
  buffer_t     *buffer;
  uint8_t      *encoded_bytes;
  size_t        encoded_length;
  uint8_t      *dns_bytes;
  size_t        dns_length;
  size_t        section_length;

  assert(driver->s != -1); /* Make sure we have a valid socket. */
  assert(data); /* Make sure they aren't trying to send NULL. */
  assert(length > 0); /* Make sure they aren't trying to send 0 bytes. */
  assert(length <= MAX_DNSCAT_LENGTH(driver->domain));

  buffer = buffer_create(BO_BIG_ENDIAN);

  /* If no domain is set, add the wildcard prefix at the start. */
  if(!driver->domain)
  {
    buffer_add_bytes(buffer, (uint8_t*)WILDCARD_PREFIX, strlen(WILDCARD_PREFIX));
    buffer_add_int8(buffer, '.');
  }

  section_length = 0;
  /* TODO: I don't much care for this loop... */
  for(i = 0; i < length; i++)
  {
    char hex_buf[3];

#ifdef WIN32
    sprintf_s(hex_buf, 3, "%02x", data[i]);
#else
    sprintf(hex_buf, "%02x", data[i]);
#endif
    buffer_add_bytes(buffer, hex_buf, 2);

    /* Add periods when we need them. */
    section_length += 2;
    if(i + 1 != length && section_length + 2 >= MAX_FIELD_LENGTH)
    {
      section_length = 0;
      buffer_add_int8(buffer, '.');
    }
  }

  /* If a domain is set, instead of the wildcard prefix, add the domain to the end. */
  if(driver->domain)
  {
    buffer_add_int8(buffer, '.');
    buffer_add_bytes(buffer, driver->domain, strlen(driver->domain));
  }
  buffer_add_int8(buffer, '\0');

  /* Get the result out. */
  encoded_bytes = buffer_create_string_and_destroy(buffer, &encoded_length);

  /* Double-check we didn't mess up the length. */
  assert(encoded_length <= MAX_DNS_LENGTH);

  dns = dns_create(_DNS_OPCODE_QUERY, _DNS_FLAG_RD, _DNS_RCODE_SUCCESS);
  dns_add_question(dns, (char*)encoded_bytes, driver->type, _DNS_CLASS_IN);
  dns_bytes = dns_to_packet(dns, &dns_length);

  LOG_INFO("Sending DNS query for: %s to %s:%d", encoded_bytes, driver->dns_host, driver->dns_port);
  udp_send(driver->s, driver->dns_host, driver->dns_port, dns_bytes, dns_length);

  safe_free(dns_bytes);
  safe_free(encoded_bytes);
  dns_destroy(dns);
}

static void handle_message(message_t *message, void *d)
{
  driver_dns_t *driver_dns = (driver_dns_t*) d;

  switch(message->type)
  {
    case MESSAGE_PACKET_OUT:
      handle_packet_out(driver_dns, message->message.packet_out.data, message->message.packet_out.length);
      break;

    default:
      LOG_FATAL("driver_dns received an invalid message!");
      abort();
  }
}

driver_dns_t *driver_dns_create(select_group_t *group, char *domain, dns_type_t type)
{
  driver_dns_t *driver_dns = (driver_dns_t*) safe_malloc(sizeof(driver_dns_t));

  /* Create the actual DNS socket. */
  LOG_INFO("Creating UDP (DNS) socket");
  driver_dns->s = udp_create_socket(0, "0.0.0.0");
  if(driver_dns->s == -1)
  {
    LOG_FATAL("Couldn't create UDP socket!");
    exit(1);
  }

  /* Set the domain and stuff. */
  driver_dns->domain   = domain;
  driver_dns->type     = type;

  /* If it succeeds, add it to the select_group */
  select_group_add_socket(group, driver_dns->s, SOCKET_TYPE_STREAM, driver_dns);
  select_set_recv(group, driver_dns->s, recv_socket_callback);
  select_set_closed(group, driver_dns->s, dns_data_closed);

  /* Subscribe to the messages we care about. */
  message_subscribe(MESSAGE_PACKET_OUT, handle_message, driver_dns);

  /* TODO: Do I still need this? */
  message_post_config_int("max_packet_length", MAX_DNSCAT_LENGTH(driver_dns->domain));

  return driver_dns;
}

void driver_dns_destroy(driver_dns_t *driver)
{
  if(driver->dns_host)
    safe_free(driver->dns_host);
  safe_free(driver);
}
