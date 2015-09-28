#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

int main(int argc, char *argv[])
{
  int blocks;
  int values_per_block;
  int fd;
  uint8_t *pBlock;
  int i;

  if ((argc != 3) && (argc != 4)) {
    printf("Usage: %s <device_name> <size of block> [<number of blocks>]\n", argv[0]);
    return 1;
  }

  values_per_block = atoi(argv[2]);
  blocks = (argc == 4) ? atoi(argv[3]) : 1;
  printf("Reading %d blocks, size %d\n", blocks, values_per_block);
  pBlock = malloc(values_per_block * 2);

  if (!pBlock) {
    printf("Failed to allocate block memory\n");
    goto failed_memalloc;
  }

  fd = open(argv[1], O_RDONLY);
  if (fd < 0) {
    printf("Failed to open %s due to: %s\n", argv[1], strerror(errno));
    goto failed_open;
  }
  for (i=0; i < blocks; i++) {
    int bytes_read = 0;

    do {
      int loop_bytes_read = read(fd, pBlock, values_per_block * 2);

      printf("read() %d bytes\n", loop_bytes_read);
      if (loop_bytes_read < 0) {
	printf("Error, %s\n", strerror(errno));
	break;
      }
      else if (loop_bytes_read == 0) {
	printf("Encountered EOF\n");
	break;
      }
      bytes_read += loop_bytes_read;
      if (bytes_read == (values_per_block * 2)) {
	uint8_t *ofs = pBlock;
	int j;

	printf("Block data:\n");
	for (j=0; j < values_per_block; j++, ofs += 2)
	  printf("%d ", (*ofs << 8) + *(ofs+1));
	printf("\n");
	break;
      }
    } while (1);
  }
  free(pBlock);
  close(fd);
  printf("Done!\n");
  return 0;



  failed_open:
  free(pBlock);
  failed_memalloc:
  return 1;
}
