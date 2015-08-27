union val {
  int intval;
  char *str;
};


void smartio_raw_to_string(int ix, void* raw_value, char *result);
void smartio_string_to_raw(int ix, const char* str, char *raw, int *raw_len);


void write_val_to_buffer(char *buf, int *len, int type, union val value);
