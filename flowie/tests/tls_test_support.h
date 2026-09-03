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
    "MIIC/TCCAeWgAwIBAgIUAOM/MuFKMgAn5TASMZSYCwKlNT8wDQYJKoZIhvcNAQEL\n"
    "BQAwADAeFw0yNjA5MDMxNzIzMTRaFw0zNjA4MzExNzIzMTRaMAAwggEiMA0GCSqG\n"
    "SIb3DQEBAQUAA4IBDwAwggEKAoIBAQCbxmbRAj2Dwky7YxPq9sKAyTX/1qNmEivJ\n"
    "hQLsZS1jsXrpF6C0xlZVjP4/8TKMljMht2LoZfMyJvo+qfOvxq/4n9zIhRyQIkVc\n"
    "8qm2+PdmhAxcPH7SkUAJJBvKfrORo2i642QEQyWHoYo/CCb8Y82E15jmj0v6heW+\n"
    "CDLGu7wzFCkms0EZgar5xnvK4230spoiMNxLTKR5Jp4rVd5nl7iyio2FLEYSnKWs\n"
    "LQC+juQ9u59jSZ2B323hRJ+oe5fP61oSMMLC4HvjmKzPUdeBr/9cMxt9bPFWgsuR\n"
    "NFigUWQRn5zgzVNkz5g8zYahwpN9uYKqcg7T2nKoLLo3K/w/IIjHAgMBAAGjbzBt\n"
    "MB0GA1UdDgQWBBTgAcV62mFSFt/bo44sA4IctWxm6zAfBgNVHSMEGDAWgBTgAcV6\n"
    "2mFSFt/bo44sA4IctWxm6zAPBgNVHRMBAf8EBTADAQH/MBoGA1UdEQQTMBGCCWxv\n"
    "Y2FsaG9zdIcEfwAAATANBgkqhkiG9w0BAQsFAAOCAQEAPJBjaGa4hEJDS4P/iFoq\n"
    "yhZI+ILck9YAQwjkj4DYJeeO7o3UXsLUu7CiM3NcSyuKR886Ro4boFc4Nsew3YVi\n"
    "/PTsqlfGKaIE7ljQR22cx246Amg1YYW5UDQQ8PTcSF6gWCccdWA/ihkj/zZjJunZ\n"
    "4+FLIspVTfq00fJgCkHZInYp/b+bCOom6iGtfo9XrTfYvtGQTSgQXW1XhKU76rs+\n"
    "yl+JvWqRxLO3cA6wIjvzmro5tgTcTk7KNlRAD0LCt9yiRzKUoUoIVgy09WvAUARf\n"
    "0zhWsU3ao+A2znm9uh2L11DMUF/ZyUT8G2stC1UGwlGjes85S8Y6Yme/69YBUvba\n"
    "rQ==\n"
    "-----END CERTIFICATE-----\n";

static const char s_tls_test_key_pem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCbxmbRAj2Dwky7\n"
    "YxPq9sKAyTX/1qNmEivJhQLsZS1jsXrpF6C0xlZVjP4/8TKMljMht2LoZfMyJvo+\n"
    "qfOvxq/4n9zIhRyQIkVc8qm2+PdmhAxcPH7SkUAJJBvKfrORo2i642QEQyWHoYo/\n"
    "CCb8Y82E15jmj0v6heW+CDLGu7wzFCkms0EZgar5xnvK4230spoiMNxLTKR5Jp4r\n"
    "Vd5nl7iyio2FLEYSnKWsLQC+juQ9u59jSZ2B323hRJ+oe5fP61oSMMLC4HvjmKzP\n"
    "UdeBr/9cMxt9bPFWgsuRNFigUWQRn5zgzVNkz5g8zYahwpN9uYKqcg7T2nKoLLo3\n"
    "K/w/IIjHAgMBAAECggEACdePug3oe0SRjfD1GoFyIypTV8sLMnd2CvrB7aCsE2JS\n"
    "/M4wdt6yW4Zke/+OLShb3UbI04K5fZMArAJKslgdC/OoKSBfGeybmFAZ3ZGLKnZJ\n"
    "kTxRp2LKlA+srjvlT/YH0TyghM2fRE7P1sRGyxrp91/N083JXaM/Gdh+KMtGWUdU\n"
    "y2CL+Y9BxTKSrSHPsfyS77x6HoUdCSVMEwzeX9yp7q+1Pw7WoBFDnUs5h++RfLOk\n"
    "ppXP+hs+uXwYN1NVs72fbBrXlVJ3d5WTig1AVwcCid8PLEzJwGLme4TxR80HElyo\n"
    "0U0Bu16Nx+ZwjjmEPRuN7qbMVE+ungRcHPd0357v3QKBgQDYvukY+YB28gS27oYU\n"
    "1ivznom8lswrVrAbuxhpwEdB9m8YlQAVAi/g8s1XrhuWx9rVKKNQh+KvusPgB7nm\n"
    "XiRQX1KiKmmP6RvuZv4TrYTj7yJIlH4Elngg2+USuROBqq4KDaYwiKVmJuObdJKq\n"
    "j5PM8+VO9452u6Wtv6l1XJ+7BQKBgQC3/Ky/2Z7uGj8FwJfunOlBEhj0AdS2f65a\n"
    "5fJS9PbqmTXnVFYz9qHMELyQUWqdXYXyYgQcTOAF5ACgwEv2tXZYhjURGSHbZmNm\n"
    "b0CRl0LeYxfrLcjLQTqFg09cEKIeA8SMJNp+2fI427qLb6Hslr03Z1THuvW8bFi4\n"
    "nI61zi42WwKBgGKOLJ/Wk7uCQPKNcxp2aHXWWPsP9raeLGXvpSIw9DXiQJIE7oye\n"
    "+fZncUe1O4ZMSg0y9U/g+gq9+eMcoENH+2swbPqgHm/5p2G4Iz//XrXmPsiR/NtJ\n"
    "MEaAtqDU1zHW0lxv6GmZONxCQqylTSuRLbu8C0DlADqtCKEAzaU4AMmhAoGAGj8E\n"
    "UbX0sxixZturkNF3gN3ZC237bzFVqQfmgqkJVDwY8XAZf/4t5JN6osfKgrplskcD\n"
    "ORpKVuzIniXwcDhAsq4qgc7pAohfo6w5NMu0MU9HiTgVzdD3TTaHKqYAynFVJ3zW\n"
    "YoQqxqupt9xr5/k08uNdt8RW69M08Nj8jrjj5MMCgYAKj8TSJycv2s9WaNBaBtyI\n"
    "xcsfp5fL6gkDehIpAKp1NAJgDrvQng9uG77ZLwV/5jOarPZ+9lQN9TnKiA2oZ7oZ\n"
    "KWegXGWLNZjR+ODDhEHTzzVdjWo7z0Iwj8cDQkpfSjZ81DWcNDwFMun1Fns++G3r\n"
    "haj7KDfY6jGKceBpTkYOzg==\n"
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
  return tls_test_write_temp_file(path, path_len, "ca", s_tls_test_cert_pem);
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
