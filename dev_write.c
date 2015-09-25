#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char *argv[])
{
  int blocks;
  int values_per_block;
  int fd;
  char *pBlock;
  int cur_value = 0;
  int i;

  if ((argc != 3) && (argc != 4)) {
    printf("Usage: %s <device_name> <size of block> [<number of blocks>]\n", argv[0]);
    return 1;
  }

  values_per_block = atoi(argv[2]);
  blocks = (argc == 4) ? atoi(argv[3]) : 1;
  pBlock = malloc(values_per_block * 2);

  if (!pBlock) {
    printf("Failed to allocate block memory\n");
    goto failed_memalloc;
  }

  fd = open(argv[1], O_WRONLY);
  if (fd < 0) {
    printf("Failed to open %s due to: %s\n", argv[1], strerror(errno));
    goto failed_open;
  }
  for (i=0; i < blocks; i++) {
    char *ofs = pBlock;
    int bytes_written;
    int j;

    for (j=0; j < values_per_block; j++, cur_value++) {
      *ofs++ = cur_value >> 8;
      *ofs++ = cur_value;
    }
    bytes_written = write(fd, pBlock, values_per_block * 2);
    if (bytes_written < 0) {
      printf("Failed to write %s due to: %s\n", argv[1], strerror(errno));
      goto failed_write;
    }
    else if (bytes_written != (values_per_block*2)) {
      printf("Partial write. Requested %d bytes, got %d\n", values_per_block*2, bytes_written);
      goto failed_write;
    }
  }
  return 0;

  failed_write:
    close(fd);
  failed_open:
  free(pBlock);
  failed_memalloc:
  return 1;
}
