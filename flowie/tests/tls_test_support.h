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

/* Test-only localhost keypair. It is never installed or used by production code. */
static const char s_tls_test_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDVjCCAj6gAwIBAgIUXmMBcNfRv8UUbtIvZWH6mfyBcuAwDQYJKoZIhvcNAQEL\n"
    "BQAwITEfMB0GA1UEAwwWQ0hUVFAgUlNBIFRlc3QgUm9vdCBDQTAeFw0yNjEwMDMy\n"
    "MTA4MTdaFw0zNjA5MzAyMTA4MTdaMBQxEjAQBgNVBAMMCWxvY2FsaG9zdDCCASIw\n"
    "DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALZZt4ryi/8X5tNLRNXqC2n9sTfM\n"
    "lafqDNruY5acicUYRwrKSHO3igGXPIgWEeeL7dlT/pT4CiBc16OtP/IqMQxYMYGK\n"
    "IwkQHQVTlHAlSs197Xml4mq6feFv12sxUvZNNk1jNM1eUlm3/4JIZ3LOD8GUlzRd\n"
    "Sa79xRIo1IhqzJe6mmqpdbC0n7WQCaR6SPRBkQVuUIQdYIYqOkH3k1cTayY4wQSS\n"
    "EHgcqVY7niWvP39Cbfk0J7Z2/0wbiwwU/CQCCCODCL8asV4C0whhCTk8RTBABoyB\n"
    "5x7LkclLZ2oTYf5rQUfRB3Sv2zm2Wd/J1H0ndgkvHLmRU2GdzHAj1xGX4N0CAwEA\n"
    "AaOBkjCBjzAMBgNVHRMBAf8EAjAAMA4GA1UdDwEB/wQEAwIFoDATBgNVHSUEDDAK\n"
    "BggrBgEFBQcDATAaBgNVHREEEzARgglsb2NhbGhvc3SHBH8AAAEwHQYDVR0OBBYE\n"
    "FKilzOfOpQ/xD6OcRnO1NemU/mf0MB8GA1UdIwQYMBaAFCIVNMoPCUzhJUhcThWQ\n"
    "o76yN557MA0GCSqGSIb3DQEBCwUAA4IBAQCiOPLASKySWhwob0T6rSjJSHh2I/nn\n"
    "Fx1NZEIqZafCmuGsuvvSaBWwQ72cuZOvyErZWng/8rxbrPR1CetZc8j8qoE+Rq6u\n"
    "06131BIwxZyYVpO0FowJWWXhHlQemq1G0zxGRUA5WRRlLOtXTA12KkeHSG6S1+44\n"
    "wW+iJweL1IsEH+s5duh9Y0Rz3vHDM8sixOfzcTQj8zdFBN7F5tZ+Qjb4HY3d2bVx\n"
    "o2bCCIuXewLzvXWeSKn990X/IpBSWGj8W1pTG17JW0g+EnMJ5+rrg32NoUuyJdmr\n"
    "KnJNFjxg9KfCp3htpIWrnSdtKYHRkV0s+ICel/Y9oEQSHXwDGReWukpy\n"
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
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQC2WbeK8ov/F+bT\n"
    "S0TV6gtp/bE3zJWn6gza7mOWnInFGEcKykhzt4oBlzyIFhHni+3ZU/6U+AogXNej\n"
    "rT/yKjEMWDGBiiMJEB0FU5RwJUrNfe15peJqun3hb9drMVL2TTZNYzTNXlJZt/+C\n"
    "SGdyzg/BlJc0XUmu/cUSKNSIasyXuppqqXWwtJ+1kAmkekj0QZEFblCEHWCGKjpB\n"
    "95NXE2smOMEEkhB4HKlWO54lrz9/Qm35NCe2dv9MG4sMFPwkAggjgwi/GrFeAtMI\n"
    "YQk5PEUwQAaMgecey5HJS2dqE2H+a0FH0Qd0r9s5tlnfydR9J3YJLxy5kVNhncxw\n"
    "I9cRl+DdAgMBAAECggEABLwmJ7G+IGDYwc5PyoUyCC3J+wKxNSO+8zVi+R919Vv4\n"
    "xkcz4+tVZk6sx/TYwKwxwCWt8+TP8ICwqcOfKDTfwqwMlwpE+CCGvGNr9JCHVXgq\n"
    "YTEFRNO0XpT2oUYt2w7stMw5bAbBuSottI6YPxMqNhLbbUTp26Ujx6GvSR6K0vq+\n"
    "WWYpq6ocVeEAuJWKrIm2Sn8M0CfooLi1ouU1EAeT8hoMiER7xEljA+PBOKQt0Zpn\n"
    "N0Vuhrh4r3Gr+Mq1okqye9iCkvBnaFsUNE9DkozrXr5J8dNPOybxwSBP5H13sMOT\n"
    "b1HgLg/h4zmmUhd5MuFjtCnmvywExQqqtHzYD0e5uQKBgQD8ttem9IdcDIiLggQo\n"
    "dCKpNDHMGrXGRvEwg5TcgFVmJDCgQR3KpwAeyNyVQDSGWyVmUZwB1JYDXQtwzw6Z\n"
    "KqeRqAbqu1wESqikjt2/2HQZSwJXXXCIRYAbnNF9ZQM8TtTmrcJ3bgAL4Gb0t7Vf\n"
    "Ni7tzapTbft1sTj4ZxF2jL6f6QKBgQC4uKtS4RDeuJX5xxvdnvBo5Tr3Yxc6ef3T\n"
    "iMeWC9yZTudtGTLnfflU/nTeVuIwlJEKj9dcR1i0pCGHHrkTNOALN4cJJk9qgeKd\n"
    "liM3DZfukcOd8Sn1NnY8gnBb2jXqmyA72pVtGOrc1YZfI/lWHzSeHNCBYIfsC93t\n"
    "eRmD9wW01QKBgBpSSP/gtECIJaiyMBCul/WbtBwYJBq0xOZSriyRSVPMLSxj2+CB\n"
    "qGeMcD0zZMUhtTGFCoS+SamsBUcIqfKWGMH5CE9mmFzyEcKiBsK2ntELmQDUgKYQ\n"
    "5xj5KnCvFrkvVMl6C6k40dRq2Q5gHAoAsL6gDz+FUw2dGdv1kEISwsARAoGAVhoe\n"
    "IZ+vEWHQ5oCcyIzRWtOezLlvll5Opk9XWOVmij9xODVKuQTLN4b0Sr8M58AYa2fo\n"
    "5qaK/oiS6Dy13/IBjLjxRJ6K3gZ07yeQepTngNiI6hIDkL02F7ZlUStBfn2YZM3a\n"
    "YOQAdbdBzw8hpbcd+6VKu0OXl7tgUtTjw9Nc4ZECgYBn+fciDdVIj+MB7KTQKIH+\n"
    "b/b4u/ueilmFdh/CwOfEKDaDGKHGVBJ74sINDLFTHVnVZCyu+aWKUX2CK+mUrC86\n"
    "UPRfH282y+mkwFv4Zuu5Y1DhTYtlhS2AzjYPnxT+xN7ix3M198BKzn4gHiI4qhAK\n"
    "th8JxHVsF9AEP3GTrIh/sA==\n"
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
  if (tls_test_write_temp_file(cert_path, cert_path_len, "crt", s_tls_test_cert_pem) != 0)
    return -1;
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
