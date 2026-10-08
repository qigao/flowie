#ifndef FLOWIE_TLS_TEST_SUPPORT_H
#define FLOWIE_TLS_TEST_SUPPORT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#ifdef _WIN32
typedef SOCKET test_socket_t;
#  define TEST_INVALID_SOCKET INVALID_SOCKET
#  define test_close_socket closesocket
#else
typedef int test_socket_t;
#  define TEST_INVALID_SOCKET (-1)
#  define test_close_socket close
#endif

static int tls_test_init_socket_runtime(void) {
#ifdef _WIN32
  static int ready = 0;
  if (ready) return 0;
  {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
  }
  ready = 1;
#endif
  return 0;
}

static int tls_test_prepare_listener(test_socket_t *listen_socket, unsigned short *port) {
  struct sockaddr_in address;
#ifdef _WIN32
  int address_size = (int)sizeof(address);
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  int reuse = 1;
  if (!listen_socket || !port || tls_test_init_socket_runtime() != 0) return -1;
  *listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*listen_socket == TEST_INVALID_SOCKET) return -1;
  (void)setsockopt(*listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(0);
  if (bind(*listen_socket, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(*listen_socket, 1) != 0 ||
      getsockname(*listen_socket, (struct sockaddr *)&address, &address_size) != 0) {
    test_close_socket(*listen_socket);
    *listen_socket = TEST_INVALID_SOCKET;
    return -1;
  }
  *port = ntohs(address.sin_port);
  return 0;
}

/* Test-only P-256 CA and localhost leaf (2026-2036), never installed.
 * The leaf has CA=false, SAN localhost/127.0.0.1/::1, and server/client EKU.
 * A CA certificate must not double as the TLS endpoint certificate. */
static const char s_tls_test_ca_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBfTCCASOgAwIBAgIUe7nhE767sa6vXvFNmy2XZBXSbJcwCgYIKoZIzj0EAwIw\n"
    "GzEZMBcGA1UEAwwQRmxvd2llIFRlc3QgUm9vdDAeFw0yNjAxMDEwMDAwMDBaFw0z\n"
    "NjAxMDEwMDAwMDBaMBsxGTAXBgNVBAMMEEZsb3dpZSBUZXN0IFJvb3QwWTATBgcq\n"
    "hkjOPQIBBggqhkjOPQMBBwNCAAQ4zR1k+kxVa8k1PPPCtvZSP6/M+nmt0aFvCvAP\n"
    "7J71uS7ygiBvQVf4ZWWcoZju29x+Z9Xy4alb7ZcYFimv1afuo0UwQzASBgNVHRMB\n"
    "Af8ECDAGAQH/AgEAMA4GA1UdDwEB/wQEAwIBBjAdBgNVHQ4EFgQUb8Ww4/ETnPoj\n"
    "fMwiI6QDRqNvVL8wCgYIKoZIzj0EAwIDSAAwRQIhALeJE82KOFX7ZtmLTD9tjW5G\n"
    "7KI75ul8SA3wGyLq2Ty3AiAtwchaAkdS76P1NeCNQ6sTsqydCCwbm3I0kO2RTGbZ\n"
    "tw==\n"
    "-----END CERTIFICATE-----\n";

static const char s_tls_test_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIB3zCCAYagAwIBAgIUPjqbDmMQ7vQZGL77amLuBqKxLeQwCgYIKoZIzj0EAwIw\n"
    "GzEZMBcGA1UEAwwQRmxvd2llIFRlc3QgUm9vdDAeFw0yNjAxMDEwMDAwMDBaFw0z\n"
    "NjAxMDEwMDAwMDBaMBQxEjAQBgNVBAMMCWxvY2FsaG9zdDBZMBMGByqGSM49AgEG\n"
    "CCqGSM49AwEHA0IABKe462/vahoTZVYUWj7dESj/L2GvvhgckOLBDVSKmWxWyXlC\n"
    "nrINzqt9XuxSghE3TykpFAS1si0RscKdP6S56l2jga4wgaswDAYDVR0TAQH/BAIw\n"
    "ADAOBgNVHQ8BAf8EBAMCB4AwHQYDVR0lBBYwFAYIKwYBBQUHAwEGCCsGAQUFBwMC\n"
    "MCwGA1UdEQQlMCOCCWxvY2FsaG9zdIcEfwAAAYcQAAAAAAAAAAAAAAAAAAAAATAd\n"
    "BgNVHQ4EFgQUFgyHg43p9SgHpJ8FQXH1f28z11kwHwYDVR0jBBgwFoAUb8Ww4/ET\n"
    "nPojfMwiI6QDRqNvVL8wCgYIKoZIzj0EAwIDRwAwRAIgBetkXpnUzkLnFkFuCNJa\n"
    "550cneGBnsL4Yp9IeKnjXf8CIFo4MVCxrb/zssvQEedceaPXHs31jExsEk6yyC7g\n"
    "udsc\n"
    "-----END CERTIFICATE-----\n";


static const char s_tls_test_ca_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDNjCCAh6gAwIBAgIUCwYd13pGVLQm0MfKu84XUDbdTkowDQYJKoZIhvcNAQEL\n"
    "BQAwITEfMB0GA1UEAwwWQ0hUVFAgUlNBIFRlc3QgUm9vdCBDQTAeFw0yNjEwMDMy\n"
    "MTA4MTdaFw0zNjA5MzAyMTA4MTdaMCExHzAdBgNVBAMMFkNIVFRQIFJTQSBUZXN0\n"
    "IFJvb3QgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC94d5ObiCS\n"
    "/yrpudSbnKaqnvf4viCmsw1L6mrE5v/RNf0r3GXfF3Kq7Zhf6X5qo1G40M3fzuD2\n"
    "opV/EztClyaQVEeZFiut+jq0hi8qBr8fz+i/M/R8kwc94knFwFQMStjeCNZx4luH\n"
    "56pl/DxAHN3oXStW/gwTZTNu5eUzFJB3S87XRSKNvJzrVWCtlA87urKA66LaXrL+\n"
    "dCqMmwR7i/7p9uWVVM6Ndy1oEGlsXDcgLumSBCCXrOAfT407Uv+Do0oEl61Sg1M+\n"
    "Av5VThM6ziomKEuogI9kFcQz6oZTUMsru5mOD0xnSJkERG9FQD6r2Ip97g34SCyD\n"
    "PZysnTHsux1fAgMBAAGjZjBkMBIGA1UdEwEB/wQIMAYBAf8CAQEwDgYDVR0PAQH/\n"
    "BAQDAgEGMB0GA1UdDgQWBBQiFTTKDwlM4SVIXE4VkKO+sjeeezAfBgNVHSMEGDAW\n"
    "gBQiFTTKDwlM4SVIXE4VkKO+sjeeezANBgkqhkiG9w0BAQsFAAOCAQEAQkrmnKrL\n"
    "mwh2v+pX7ohQYrnwDo5+ftCS0DEbWeHnnkS6pqdEOtCYu/UciPpgzWJ4f1snP+xB\n"
    "KJfcTvsbFgHuN2/FkZDOIznDLxBQFLqKTZwsmHE9yCITUJ+nWBZFwGdh9Ev+S2xX\n"
    "+tqmOA00lcjzWQ2EB5Gx+yGKvmDbg6E83ifyTV68m83UESFXkohB4Ipt4hWrWB+i\n"
    "srkq+6V/BbJgvBwjoblC3L4uiNqzo5OPB5ylGBThVDOF3thH3eRZBPy0+1fnutWe\n"
    "NIc7n3IGtBOZmadKmrYYlohg3wRZhoJMl2IFtQvjNrS5IcpDkAgJw5vKZod3Wgm3\n"
    "XzvofX71hj/B9g==\n"
    "-----END CERTIFICATE-----\n";


static const char s_tls_test_key_pem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgmkcTsmw218MDZ+8y\n"
    "LehXLcgQGph2PHQj2wpRgEKxz1yhRANCAASnuOtv72oaE2VWFFo+3REo/y9hr74Y\n"
    "HJDiwQ1UiplsVsl5Qp6yDc6rfV7sUoIRN08pKRQEtbItEbHCnT+kuepd\n"
    "-----END PRIVATE KEY-----\n";

static void tls_test_remove_file(const char *path) {
  if (path && path[0] != '\0') (void)remove(path);
}

static int tls_test_write_temp_file(char *path, size_t path_len, const char *tag,
                                    const char *contents) {
  FILE *fp;
  size_t contents_len;
  int write_ok;
  if (!path || path_len == 0 || !tag || !contents) return -1;
  path[0] = '\0';
#ifdef _WIN32
  {
    char temp_dir[MAX_PATH];
    char temp_file[MAX_PATH];
    DWORD dir_len = GetTempPathA((DWORD)sizeof(temp_dir), temp_dir);
    if (dir_len == 0 || dir_len >= sizeof(temp_dir)) return -1;
    if (GetTempFileNameA(temp_dir, tag, 0, temp_file) == 0) return -1;
    if (strlen(temp_file) + 1 > path_len) {
      (void)DeleteFileA(temp_file);
      return -1;
    }
    memcpy(path, temp_file, strlen(temp_file) + 1);
  }
#else
  {
    char temp_file[128];
    int written = snprintf(temp_file, sizeof(temp_file), "/tmp/flowie_tls_%s_XXXXXX", tag);
    int fd;
    if (written < 0 || (size_t)written >= sizeof(temp_file)) return -1;
    fd = mkstemp(temp_file);
    if (fd < 0) return -1;
    (void)close(fd);
    if (strlen(temp_file) + 1 > path_len) {
      (void)unlink(temp_file);
      return -1;
    }
    memcpy(path, temp_file, strlen(temp_file) + 1);
  }
#endif
  fp = fopen(path, "wb");
  if (!fp) {
    tls_test_remove_file(path);
    path[0] = '\0';
    return -1;
  }
  contents_len = strlen(contents);
  write_ok = fwrite(contents, 1, contents_len, fp) == contents_len;
  if (fclose(fp) != 0) write_ok = 0;
  if (!write_ok) {
    tls_test_remove_file(path);
    path[0] = '\0';
    return -1;
  }
  return 0;
}

static int tls_test_write_ca_file(char *path, size_t path_len) {
  return tls_test_write_temp_file(path, path_len, "ca", s_tls_test_ca_pem);
}

static int tls_test_write_server_files(char *cert_path, size_t cert_path_len,
                                       char *key_path, size_t key_path_len) {
  FILE *chain;
  int chain_ok;
  if (tls_test_write_temp_file(cert_path, cert_path_len, "crt", s_tls_test_cert_pem) != 0)
    return -1;
  /* Some fixtures also use this file as their trust bundle. Keep the leaf first
   * for server identity and append its actual trust anchor. */
  chain = fopen(cert_path, "ab");
  chain_ok = chain != NULL;
  if (chain != NULL) {
    chain_ok = fwrite(s_tls_test_ca_pem, 1u, sizeof(s_tls_test_ca_pem) - 1u, chain) ==
               sizeof(s_tls_test_ca_pem) - 1u;
    if (fclose(chain) != 0) chain_ok = 0;
  }
  if (!chain_ok) {
    tls_test_remove_file(cert_path);
    cert_path[0] = '\0';
    return -1;
  }
  if (tls_test_write_temp_file(key_path, key_path_len, "key", s_tls_test_key_pem) != 0) {
    tls_test_remove_file(cert_path);
    cert_path[0] = '\0';
    return -1;
  }
  return 0;
}

static int tls_test_set_ca_file_env(const char *path) {
  if (!path || path[0] == '\0') return -1;
#ifdef _WIN32
  if (_putenv_s("SALTS_TLS_CA_FILE", path) != 0) return -1;
  if (_putenv_s("SALTS_TLS_CA_PATH", "") != 0) return -1;
#else
  if (setenv("SALTS_TLS_CA_FILE", path, 1) != 0) return -1;
  if (unsetenv("SALTS_TLS_CA_PATH") != 0) return -1;
#endif
  return 0;
}

static int tls_test_set_server_env(const char *cert_path, const char *key_path) {
  if (!cert_path || cert_path[0] == '\0' || !key_path || key_path[0] == '\0') return -1;
#ifdef _WIN32
  if (_putenv_s("SALTS_TLS_CERT_FILE", cert_path) != 0) return -1;
  if (_putenv_s("SALTS_TLS_KEY_FILE", key_path) != 0) return -1;
#else
  if (setenv("SALTS_TLS_CERT_FILE", cert_path, 1) != 0) return -1;
  if (setenv("SALTS_TLS_KEY_FILE", key_path, 1) != 0) return -1;
#endif
  return 0;
}

static void tls_test_clear_ca_env(void) {
#ifdef _WIN32
  (void)_putenv_s("SALTS_TLS_CA_FILE", "");
  (void)_putenv_s("SALTS_TLS_CA_PATH", "");
#else
  (void)unsetenv("SALTS_TLS_CA_FILE");
  (void)unsetenv("SALTS_TLS_CA_PATH");
#endif
}

static void tls_test_clear_server_env(void) {
#ifdef _WIN32
  (void)_putenv_s("SALTS_TLS_CERT_FILE", "");
  (void)_putenv_s("SALTS_TLS_KEY_FILE", "");
#else
  (void)unsetenv("SALTS_TLS_CERT_FILE");
  (void)unsetenv("SALTS_TLS_KEY_FILE");
#endif
}

#endif
