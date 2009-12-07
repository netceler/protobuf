/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * upbc is the upb compiler.  This is some deep code that I wish could be
 * easier to understand, but by its nature it is doing some very "meta"
 * kinds of things.
 *
 * Copyright (c) 2009 Joshua Haberman.  See LICENSE for details.
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include "descriptor.h"
#include "upb_array.h"
#include "upb_def.h"
#include "upb_mm.h"
#include "upb_msg.h"
#include "upb_text.h"

/* These are in-place string transformations that do not change the length of
 * the string (and thus never need to re-allocate). */

/* Convert to C identifier: foo.bar.Baz -> foo_bar_Baz. */
static void to_cident(struct upb_string *str)
{
  for(uint32_t i = 0; i < str->byte_len; i++)
    if(str->ptr[i] == '.' || str->ptr[i] == '/')
      str->ptr[i] = '_';
}

/* Convert to C proprocessor identifier: foo.bar.Baz -> FOO_BAR_BAZ. */
static void to_preproc(struct upb_string *str)
{
  to_cident(str);
  for(uint32_t i = 0; i < str->byte_len; i++)
    str->ptr[i] = toupper(str->ptr[i]);
}

static int my_memrchr(char *data, char c, size_t len)
{
  int off = len-1;
  while(off > 0 && data[off] != c) --off;
  return off;
}

void *strtable_to_array(struct upb_strtable *t, int *size)
{
  *size = t->t.count;
  void **array = malloc(*size * sizeof(void*));
  struct upb_strtable_entry *e;
  int i = 0;
  for(e = upb_strtable_begin(t); e && i < *size; e = upb_strtable_next(t, e))
    array[i++] = e;
  assert(i == *size && e == NULL);
  return array;
}

/* The _const.h file defines the constants (enums) defined in the .proto
 * file. */
static void write_const_h(struct upb_def *defs[], int num_entries,
                          char *outfile_name, FILE *stream)
{
  /* Header file prologue. */
  struct upb_string *include_guard_name = upb_strdupc(outfile_name);
  to_preproc(include_guard_name);
  /* A bit cheesy, but will do the job. */
  include_guard_name->ptr[include_guard_name->byte_len-1] = 'C';
  fputs("/* This file was generated by upbc (the upb compiler).  "
        "Do not edit. */\n\n", stream),
  fprintf(stream, "#ifndef " UPB_STRFMT "\n", UPB_STRARG(include_guard_name));
  fprintf(stream, "#define " UPB_STRFMT "\n\n", UPB_STRARG(include_guard_name));
  fputs("#ifdef __cplusplus\n", stream);
  fputs("extern \"C\" {\n", stream);
  fputs("#endif\n\n", stream);

  /* Enums. */
  fprintf(stream, "/* Enums. */\n\n");
  for(int i = 0; i < num_entries; i++) {  /* Foreach enum */
    if(defs[i]->type != UPB_DEF_ENUM) continue;
    struct upb_enumdef *enumdef = upb_downcast_enumdef(defs[i]);
    struct upb_string *enum_name = upb_strdup(UPB_UPCAST(enumdef)->fqname);
    struct upb_string *enum_val_prefix = upb_strdup(enum_name);
    to_cident(enum_name);

    enum_val_prefix->byte_len = my_memrchr(enum_val_prefix->ptr,
                                           UPB_SYMBOL_SEPARATOR,
                                           enum_val_prefix->byte_len);
    enum_val_prefix->byte_len++;
    to_preproc(enum_val_prefix);

    fprintf(stream, "typedef enum " UPB_STRFMT " {\n", UPB_STRARG(enum_name));
    struct upb_enum_iter iter;
    bool first = true;
    /* Foreach enum value. */
    for(upb_enum_begin(&iter, enumdef); !upb_enum_done(&iter); upb_enum_next(&iter)) {
      struct upb_string *value_name = upb_strdup(iter.name);
      to_preproc(value_name);
      /* "  GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_UINT32 = 13," */
      if (!first) fputs(",\n", stream);
      first = false;
      fprintf(stream, "  " UPB_STRFMT UPB_STRFMT " = %" PRIu32,
              UPB_STRARG(enum_val_prefix), UPB_STRARG(value_name), iter.val);
      upb_string_unref(value_name);
    }
    fprintf(stream, "\n} " UPB_STRFMT ";\n\n", UPB_STRARG(enum_name));
    upb_string_unref(enum_name);
    upb_string_unref(enum_val_prefix);
  }

  /* Epilogue. */
  fputs("#ifdef __cplusplus\n", stream);
  fputs("}  /* extern \"C\" */\n", stream);
  fputs("#endif\n\n", stream);
  fprintf(stream, "#endif  /* " UPB_STRFMT " */\n", UPB_STRARG(include_guard_name));
  upb_string_unref(include_guard_name);
}

/* The .h file defines structs for the types defined in the .proto file.  It
 * also defines constants for the enum values.
 *
 * Assumes that d has been validated. */
static void write_h(struct upb_def *defs[], int num_defs, char *outfile_name,
                    char *descriptor_cident, FILE *stream)
{
  /* Header file prologue. */
  struct upb_string *include_guard_name = upb_strdupc(outfile_name);
  to_preproc(include_guard_name);
  fputs("/* This file was generated by upbc (the upb compiler).  "
        "Do not edit. */\n\n", stream),
  fprintf(stream, "#ifndef " UPB_STRFMT "\n", UPB_STRARG(include_guard_name));
  fprintf(stream, "#define " UPB_STRFMT "\n\n", UPB_STRARG(include_guard_name));
  fputs("#include <upb_struct.h>\n\n", stream);
  fputs("#ifdef __cplusplus\n", stream);
  fputs("extern \"C\" {\n", stream);
  fputs("#endif\n\n", stream);

  if(descriptor_cident) {
    fputs("struct google_protobuf_FileDescriptorSet;\n", stream);
    fprintf(stream, "extern struct google_protobuf_FileDescriptorSet *%s;\n\n",
            descriptor_cident);
  }

  /* Forward declarations. */
  fputs("/* Forward declarations of all message types.\n", stream);
  fputs(" * So they can refer to each other in ", stream);
  fputs("possibly-recursive ways. */\n\n", stream);

  for(int i = 0; i < num_defs; i++) {  /* Foreach message */
    struct upb_msgdef *m = upb_dyncast_msgdef(defs[i]);
    if(!m) continue;
    struct upb_string *msg_name = upb_strdup(UPB_UPCAST(m)->fqname);
    to_cident(msg_name);
    fprintf(stream, "struct " UPB_STRFMT ";\n", UPB_STRARG(msg_name));
    fprintf(stream, "typedef struct " UPB_STRFMT "\n    " UPB_STRFMT ";\n\n",
            UPB_STRARG(msg_name), UPB_STRARG(msg_name));
    upb_string_unref(msg_name);
  }

  /* Message Declarations. */
  fputs("/* The message definitions themselves. */\n\n", stream);
  for(int i = 0; i < num_defs; i++) {  /* Foreach message */
    struct upb_msgdef *m = upb_dyncast_msgdef(defs[i]);
    if(!m) continue;
    struct upb_string *msg_name = upb_strdup(UPB_UPCAST(m)->fqname);
    to_cident(msg_name);
    fprintf(stream, "struct " UPB_STRFMT " {\n", UPB_STRARG(msg_name));
    fputs("  struct upb_mmhead mmhead;\n", stream);
    fputs("  struct upb_msgdef *def;\n", stream);
    fputs("  union {\n", stream);
    fprintf(stream, "    uint8_t bytes[%" PRIu32 "];\n", m->set_flags_bytes);
    fputs("    struct {\n", stream);
    for(upb_field_count_t j = 0; j < m->num_fields; j++) {
      static char* labels[] = {"", "optional", "required", "repeated"};
      struct upb_fielddef *f = &m->fields[j];
      fprintf(stream, "      bool " UPB_STRFMT ":1;  /* = %" PRIu32 ", %s. */\n",
              UPB_STRARG(f->name), f->number, labels[f->label]);
    }
    fputs("    } has;\n", stream);
    fputs("  } set_flags;\n", stream);
    for(upb_field_count_t j = 0; j < m->num_fields; j++) {
      struct upb_fielddef *f = &m->fields[j];
      if(upb_issubmsg(f)) {
        struct upb_string *type_name = upb_strdup(f->def->fqname);
        to_cident(type_name);
        if(f->label == GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_LABEL_REPEATED) {
          fprintf(stream, "  UPB_MSG_ARRAY(" UPB_STRFMT ")* " UPB_STRFMT ";\n",
                  UPB_STRARG(type_name), UPB_STRARG(f->name));
        } else {
          fprintf(stream, "  " UPB_STRFMT "* " UPB_STRFMT ";\n",
                  UPB_STRARG(type_name), UPB_STRARG(f->name));
        }
        upb_string_unref(type_name);
      } else if(f->label == GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_LABEL_REPEATED) {
        static char* c_types[] = {
          "", "struct upb_double_array*", "struct upb_float_array*",
          "struct upb_int64_array*", "struct upb_uint64_array*",
          "struct upb_int32_array*", "struct upb_uint64_array*",
          "struct upb_uint32_array*", "struct upb_bool_array*",
          "struct upb_string_array*", "", "",
          "struct upb_string_array*", "struct upb_uint32_array*",
          "struct upb_uint32_array*", "struct upb_int32_array*",
          "struct upb_int64_array*", "struct upb_int32_array*",
          "struct upb_int64_array*"
        };
        fprintf(stream, "  %s " UPB_STRFMT ";\n",
                c_types[f->type], UPB_STRARG(f->name));
      } else {
        static char* c_types[] = {
          "", "double", "float", "int64_t", "uint64_t", "int32_t", "uint64_t",
          "uint32_t", "bool", "struct upb_string*", "", "",
          "struct upb_string*", "uint32_t", "int32_t", "int32_t", "int64_t",
          "int32_t", "int64_t"
        };
        fprintf(stream, "  %s " UPB_STRFMT ";\n",
                c_types[f->type], UPB_STRARG(f->name));
      }
    }
    fputs("};\n", stream);
    fprintf(stream, "UPB_DEFINE_MSG_ARRAY(" UPB_STRFMT ")\n\n",
            UPB_STRARG(msg_name));
    upb_string_unref(msg_name);
  }

  /* Epilogue. */
  fputs("#ifdef __cplusplus\n", stream);
  fputs("}  /* extern \"C\" */\n", stream);
  fputs("#endif\n\n", stream);
  fprintf(stream, "#endif  /* " UPB_STRFMT " */\n", UPB_STRARG(include_guard_name));
  upb_string_unref(include_guard_name);
}

/* Format of table entries that we use when analyzing data structures for
 * write_messages_c. */
struct strtable_entry {
  struct upb_strtable_entry e;
  int offset;
  int num;
};

struct typetable_entry {
  struct upb_strtable_entry e;
  struct upb_fielddef *field;
  struct upb_string *cident;  /* Type name converted with to_cident(). */
  /* A list of all values of this type, in an established order. */
  union upb_value *values;
  int values_size, values_len;
  struct array {
    int offset;
    int len;
    struct upb_array *ptr;  /* So we can find it later. */
  } *arrays;
  int arrays_size, arrays_len;
};

struct msgtable_entry {
  struct upb_inttable_entry e;
  void *msg;
  int num;  /* Unique offset into the list of all msgs of this type. */
};

int compare_entries(const void *_e1, const void *_e2)
{
  struct strtable_entry *const*e1 = _e1, *const*e2 = _e2;
  return upb_strcmp((*e1)->e.key, (*e2)->e.key);
}

/* Mutually recursive functions to recurse over a set of possibly nested
 * messages and extract all the strings.
 *
 * TODO: make these use a generic msg visitor. */

static void add_strings_from_msg(struct upb_msg *msg, struct upb_strtable *t);

static void add_strings_from_value(union upb_value_ptr p,
                                   struct upb_fielddef *f,
                                   struct upb_strtable *t)
{
  if(upb_isstringtype(f->type)) {
    struct strtable_entry e = {.e = {.key = *p.str}};
    if(upb_strtable_lookup(t, e.e.key) == NULL)
      upb_strtable_insert(t, &e.e);
  } else if(upb_issubmsg(f)) {
    add_strings_from_msg(*p.msg, t);
  }
}

static void add_strings_from_msg(struct upb_msg *msg, struct upb_strtable *t)
{
  struct upb_msgdef *m = msg->def;
  for(upb_field_count_t i = 0; i < m->num_fields; i++) {
    struct upb_fielddef *f = &m->fields[i];
    if(!upb_msg_isset(msg, f)) continue;
    union upb_value_ptr p = upb_msg_getptr(msg, f);
    if(upb_isarray(f)) {
      struct upb_array *arr = *p.arr;
      for(uint32_t j = 0; j < arr->len; j++)
        add_strings_from_value(upb_array_getelementptr(arr, j), f, t);
    } else {
      add_strings_from_value(p, f, t);
    }
  }
}

/* Mutually recursive functions to recurse over a set of possibly nested
 * messages and extract all the messages (keyed by type).
 *
 * TODO: make these use a generic msg visitor. */

struct typetable_entry *get_or_insert_typeentry(struct upb_strtable *t,
                                                struct upb_fielddef *f)
{
  struct upb_string *type_name = upb_issubmsg(f) ? upb_strdup(f->def->fqname) :
                                                   upb_strdupc(upb_type_info[f->type].ctype);
  struct typetable_entry *type_e = upb_strtable_lookup(t, type_name);
  if(type_e == NULL) {
    struct typetable_entry new_type_e = {
      .e = {.key = type_name}, .field = f, .cident = upb_strdup(type_name),
      .values = NULL, .values_size = 0, .values_len = 0,
      .arrays = NULL, .arrays_size = 0, .arrays_len = 0
    };
    to_cident(new_type_e.cident);
    assert(upb_strtable_lookup(t, type_name) == NULL);
    assert(upb_strtable_lookup(t, new_type_e.e.key) == NULL);
    upb_strtable_insert(t, &new_type_e.e);
    type_e = upb_strtable_lookup(t, type_name);
    assert(type_e);
  } else {
    upb_string_unref(type_name);
  }
  return type_e;
}

static void add_value(union upb_value_ptr p, struct upb_fielddef *f,
                      struct upb_strtable *t)
{
  struct typetable_entry *type_e = get_or_insert_typeentry(t, f);
  if(type_e->values_len == type_e->values_size) {
    type_e->values_size = UPB_MAX(type_e->values_size * 2, 4);
    type_e->values = realloc(type_e->values, sizeof(*type_e->values) * type_e->values_size);
  }
  type_e->values[type_e->values_len++] = upb_value_read(p, f->type);
}

static void add_submsgs(struct upb_msg *msg, struct upb_strtable *t)
{
  struct upb_msgdef *m = msg->def;
  for(upb_field_count_t i = 0; i < m->num_fields; i++) {
    struct upb_fielddef *f = &m->fields[i];
    if(!upb_msg_isset(msg, f)) continue;
    union upb_value_ptr p = upb_msg_getptr(msg, f);
    if(upb_isarray(f)) {
      if(upb_isstring(f)) continue;  /* Handled by a different code-path. */
      struct upb_array *arr = *p.arr;

      /* Add to our list of arrays for this type. */
      struct typetable_entry *arr_type_e =
          get_or_insert_typeentry(t, f);
      if(arr_type_e->arrays_len == arr_type_e->arrays_size) {
        arr_type_e->arrays_size = UPB_MAX(arr_type_e->arrays_size * 2, 4);
        arr_type_e->arrays = realloc(arr_type_e->arrays,
                                     sizeof(*arr_type_e->arrays)*arr_type_e->arrays_size);
      }
      arr_type_e->arrays[arr_type_e->arrays_len].offset = arr_type_e->values_len;
      arr_type_e->arrays[arr_type_e->arrays_len].len = arr->len;
      arr_type_e->arrays[arr_type_e->arrays_len].ptr = *p.arr;
      arr_type_e->arrays_len++;

      /* Add the individual values in the array. */
      for(uint32_t j = 0; j < arr->len; j++)
        add_value(upb_array_getelementptr(arr, j), f, t);

      /* Add submsgs.  We must do this separately so that the msgs in this
       * array are contiguous (and don't have submsgs of the same type
       * interleaved). */
      for(uint32_t j = 0; j < arr->len; j++)
        add_submsgs(*upb_array_getelementptr(arr, j).msg, t);
    } else {
      if(!upb_issubmsg(f)) continue;
      add_value(p, f, t);
      add_submsgs(*p.msg, t);
    }
  }
}

/* write_messages_c emits a .c file that contains the data of a protobuf,
 * serialized as C structures. */
static void write_message_c(struct upb_msg *msg, char *cident, char *hfile_name,
                            int argc, char *argv[], char *infile_name,
                            FILE *stream)
{
  fputs(
    "/*\n"
    " * This file is a data dump of a protocol buffer into a C structure.\n"
    " * It was created by the upb compiler (upbc) with the following\n"
    " * command-line:\n"
    " *\n", stream);
  fputs(" *   ", stream);
  for(int i = 0; i < argc; i++) {
    fputs(argv[i], stream);
    if(i < argc-1) fputs(" ", stream);
  }
  fputs("\n *\n", stream);
  fprintf(stream, " * This file is a dump of '%s'.\n", infile_name);
  fputs(
    " * It contains exactly the same data, but in a C structure form\n"
    " * instead of a serialized protobuf.  This file contains no code,\n"
    " * only data.\n"
    " *\n"
    " * This file was auto-generated.  Do not edit. */\n\n", stream);

  fprintf(stream, "#include \"%s\"\n\n", hfile_name);

  /* Gather all strings into a giant string.  Use a hash to prevent adding the
   * same string more than once. */
  struct upb_strtable strings;
  upb_strtable_init(&strings, 16, sizeof(struct strtable_entry));
  add_strings_from_msg(msg, &strings);

  int size;
  struct strtable_entry **str_entries = strtable_to_array(&strings, &size);
  /* Sort for nice size and reproduceability. */
  qsort(str_entries, size, sizeof(void*), compare_entries);

  /* Emit strings. */
  fputs("static char strdata[] =\n  \"", stream);
  int col = 2;
  int offset = 0;
  for(int i = 0; i < size; i++) {
    struct upb_string *s = str_entries[i]->e.key;
    str_entries[i]->offset = offset;
    str_entries[i]->num = i;
    for(uint32_t j = 0; j < s->byte_len; j++) {
      if(++col == 80) {
        fputs("\"\n  \"", stream);
        col = 3;
      }
      fputc(s->ptr[j], stream);
    }
    offset += s->byte_len;
  }
  fputs("\";\n\n", stream);

  fputs("static struct upb_string strings[] = {\n", stream);
  for(int i = 0; i < size; i++) {
    struct strtable_entry *e = str_entries[i];
    fprintf(stream, "  {.ptr = &strdata[%d], .byte_len=%d},\n", e->offset, e->e.key->byte_len);
  }
  fputs("};\n\n", stream);
  free(str_entries);

  /* Gather a list of types for which we are emitting data, and give each msg
   * a unique number within its type. */
  struct upb_strtable types;
  upb_strtable_init(&types, 16, sizeof(struct typetable_entry));
  union upb_value val = {.msg = msg};
  /* A fake field to get the recursion going. */
  struct upb_fielddef fake_field = {
      .type = UPB_TYPE(MESSAGE),
      .def = UPB_UPCAST(msg->def),
  };
  add_value(upb_value_addrof(&val), &fake_field, &types);
  add_submsgs(msg, &types);

  /* Emit foward declarations for all msgs of all types, and define arrays. */
  fprintf(stream, "/* Forward declarations of messages, and array decls. */\n");
  struct typetable_entry *e = upb_strtable_begin(&types);
  for(; e; e = upb_strtable_next(&types, &e->e)) {
    fprintf(stream, "static " UPB_STRFMT " " UPB_STRFMT "_values[%d];\n\n",
            UPB_STRARG(e->cident), UPB_STRARG(e->cident), e->values_len);
    if(e->arrays_len > 0) {
      fprintf(stream, "static " UPB_STRFMT " *" UPB_STRFMT "_array_elems[] = {\n",
              UPB_STRARG(e->cident), UPB_STRARG(e->cident));
      for(int i = 0; i < e->arrays_len; i++) {
        struct array *arr = &e->arrays[i];
        for(int j = 0; j < arr->len; j++)
          fprintf(stream, "    &" UPB_STRFMT "_values[%d],\n", UPB_STRARG(e->cident), arr->offset + j);
      }
      fprintf(stream, "};\n");

      int cum_offset = 0;
      fprintf(stream, "static UPB_MSG_ARRAY(" UPB_STRFMT ") " UPB_STRFMT "_arrays[%d] = {\n",
              UPB_STRARG(e->cident), UPB_STRARG(e->cident), e->arrays_len);
      for(int i = 0; i < e->arrays_len; i++) {
        struct array *arr = &e->arrays[i];
        fprintf(stream, "  {.elements = &" UPB_STRFMT "_array_elems[%d], .len=%d},\n",
                UPB_STRARG(e->cident), cum_offset, arr->len);
        cum_offset += arr->len;
      }
      fprintf(stream, "};\n");
    }
  }

  /* Emit definitions. */
  for(e = upb_strtable_begin(&types); e; e = upb_strtable_next(&types, &e->e)) {
    fprintf(stream, "static " UPB_STRFMT " " UPB_STRFMT "_values[%d] = {\n\n",
            UPB_STRARG(e->cident), UPB_STRARG(e->cident), e->values_len);
    for(int i = 0; i < e->values_len; i++) {
      union upb_value val = e->values[i];
      if(upb_issubmsg(e->field)) {
        struct upb_msgdef *m = upb_downcast_msgdef(e->field->def);
        void *msgdata = val.msg;
        /* Print set flags. */
        fputs("  {.set_flags = {.has = {\n", stream);
        for(upb_field_count_t j = 0; j < m->num_fields; j++) {
          struct upb_fielddef *f = &m->fields[j];
          fprintf(stream, "    ." UPB_STRFMT " = ", UPB_STRARG(f->name));
          if(upb_msg_isset(msgdata, f))
            fprintf(stream, "true");
          else
            fprintf(stream, "false");
          fputs(",\n", stream);
        }
        fputs("  }},\n", stream);
        /* Print msg data. */
        for(upb_field_count_t j = 0; j < m->num_fields; j++) {
          struct upb_fielddef *f = &m->fields[j];
          union upb_value val = upb_value_read(upb_msg_getptr(msgdata, f), f->type);
          fprintf(stream, "    ." UPB_STRFMT " = ", UPB_STRARG(f->name));
          if(!upb_msg_isset(msgdata, f)) {
            fputs("0,   /* Not set. */", stream);
          } else if(upb_isstring(f)) {
            if(upb_isarray(f)) {
              fputs("Ack, string arrays are not supported yet!\n", stderr);
              exit(1);
            } else {
              struct strtable_entry *str_e = upb_strtable_lookup(&strings, val.str);
              assert(str_e);
              fprintf(stream, "&strings[%d],   /* \"" UPB_STRFMT "\" */",
                      str_e->num, UPB_STRARG(val.str));
            }
          } else if(upb_isarray(f)) {
            /* Find this submessage in the list of msgs for that type. */
            struct typetable_entry  *type_e = get_or_insert_typeentry(&types, f);
            assert(type_e);
            int arr_num = -1;
            for(int k = 0; k < type_e->arrays_len; k++) {
              if(type_e->arrays[k].ptr == val.arr) {
                arr_num = k;
                break;
              }
            }
            assert(arr_num != -1);
            fprintf(stream, "&" UPB_STRFMT "_arrays[%d],", UPB_STRARG(type_e->cident), arr_num);
          } else if(upb_issubmsg(f)) {
            /* Find this submessage in the list of msgs for that type. */
            struct typetable_entry  *type_e = get_or_insert_typeentry(&types, f);
            assert(type_e);
            int msg_num = -1;
            for(int k = 0; k < type_e->values_len; k++) {
              if(type_e->values[k].msg == val.msg) {
                msg_num = k;
                break;
              }
            }
            assert(msg_num != -1);
            fprintf(stream, "&" UPB_STRFMT "_values[%d],", UPB_STRARG(type_e->cident), msg_num);
          } else {
            upb_text_printval(f->type, val, stream);
            fputs(",", stream);
          }
          fputs("\n", stream);
        }
        fputs("  },\n", stream);
      } else if(upb_isstring(e->field)) {

      } else {
        /* Non string, non-message data. */
        upb_text_printval(e->field->type, val, stream);
      }
    }
    fputs("};\n", stream);
  }

  struct typetable_entry *toplevel_type =
      get_or_insert_typeentry(&types, &fake_field);
  assert(toplevel_type);
  fputs("/* The externally-visible definition. */\n", stream);
  /* It is always at offset zero, because we add it first. */
  fprintf(stream, UPB_STRFMT " *%s = &" UPB_STRFMT "_values[0];\n",
          UPB_STRARG(toplevel_type->cident), cident,
          UPB_STRARG(toplevel_type->cident));

  /* Free tables. */
  for(e = upb_strtable_begin(&types); e; e = upb_strtable_next(&types, &e->e)) {
    upb_string_unref(e->e.key);
    upb_string_unref(e->cident);
    free(e->values);
    free(e->arrays);
  }
  upb_strtable_free(&types);
  upb_strtable_free(&strings);
}

const char usage[] =
  "upbc -- upb compiler.\n"
  "upb v0.1  http://blog.reverberate.org/upb/\n"
  "\n"
  "Usage: upbc [options] descriptor-file\n"
  "\n"
  "  -i C-IDENFITER     Output the descriptor as a C data structure with the\n"
  "                     given identifier (otherwise only a header will be\n"
  "                     generated\n"
  "\n"
  "  -o OUTFILE-BASE    Write to OUTFILE-BASE.h and OUTFILE-BASE.c instead\n"
  "                     of using the input file as a basename.\n"
;

void usage_err(char *err)
{
  fprintf(stderr, "upbc: %s\n\n", err);
  fputs(usage, stderr);
  exit(1);
}

void error(char *err, ...)
{
  va_list args;
  va_start(args, err);
  fprintf(stderr, "upbc: ");
  vfprintf(stderr, err, args);
  va_end(args);
  exit(1);
}

void sort_fields_in_descriptor(google_protobuf_DescriptorProto *d)
{
  if(d->set_flags.has.field) upb_fielddef_sortfds(d->field->elements, d->field->len);
  if(d->set_flags.has.nested_type)
    for(uint32_t i = 0; i < d->nested_type->len; i++)
      sort_fields_in_descriptor(d->nested_type->elements[i]);
}

int main(int argc, char *argv[])
{
  /* Parse arguments. */
  char *outfile_base = NULL, *input_file = NULL, *cident = NULL;
  for(int i = 1; i < argc; i++) {
    if(strcmp(argv[i], "-o") == 0) {
      if(++i == argc)
        usage_err("-o must be followed by a FILE-BASE.");
      else if(outfile_base)
        usage_err("-o was specified multiple times.");
      outfile_base = argv[i];
    } else if(strcmp(argv[i], "-i") == 0) {
      if(++i == argc)
        usage_err("-i must be followed by a C-IDENTIFIER.");
      else if(cident)
        usage_err("-i was specified multiple times.");
      cident = argv[i];
    } else {
      if(input_file)
        usage_err("You can only specify one input file.");
      input_file = argv[i];
    }
  }
  if(!input_file) usage_err("You must specify an input file.");
  if(!outfile_base) outfile_base = input_file;

  // Read and parse input file.
  struct upb_string *descriptor = upb_strreadfile(input_file);
  if(!descriptor)
    error("Couldn't read input file.");
  struct upb_symtab *s = upb_symtab_new();
  struct upb_msg *fds_msg = upb_msg_new(s->fds_msgdef);
  struct upb_status status = UPB_STATUS_INIT;
  upb_msg_parsestr(fds_msg, descriptor->ptr, descriptor->byte_len, &status);
  if(!upb_ok(&status))
    error("Failed to parse input file descriptor: %s", status.msg);
  google_protobuf_FileDescriptorSet *fds = (void*)fds_msg;

  upb_symtab_add_desc(s, descriptor, &status);
  if(!upb_ok(&status))
    error("Failed to add descriptor: %s", status.msg);

  // We need to sort the fields of all the descriptors.  This is currently
  // somewhat special-cased to when we are emitting a descriptor for
  // FileDescriptorProto, which is used internally for bootstrapping.
  //
  // The fundamental issue is that we will be parsing descriptors into memory
  // using a reflection-based code-path, but upb then reads the descriptors
  // from memory using the C structs emitted by upbc.  This means that the
  // msgdef we will use internally to parse the descriptors must use the same
  // field order as the .h files we are about to generate.  But the msgdefs we
  // will use to generate those .h files have already been sorted according to
  // this scheme.
  //
  // If/when we ever make upbc more general, we'll have to revisit this.
  for(uint32_t i = 0; i < fds->file->len; i++) {
    google_protobuf_FileDescriptorProto *fd = fds->file->elements[i];
    if(!fd->set_flags.has.message_type) continue;
    for(uint32_t j = 0; j < fd->message_type->len; j++)
      sort_fields_in_descriptor(fd->message_type->elements[j]);
  }

  /* Emit output files. */
  const int maxsize = 256;
  char h_filename[maxsize], h_const_filename[maxsize], c_filename[maxsize];
  if(snprintf(h_filename, maxsize, "%s.h", outfile_base) >= maxsize ||
     snprintf(c_filename, maxsize, "%s.c", outfile_base) >= maxsize ||
     snprintf(h_const_filename, maxsize, "%s_const.h", outfile_base) >= maxsize)
    error("File base too long.\n");

  FILE *h_file = fopen(h_filename, "w");
  if(!h_file) error("Failed to open .h output file");
  FILE *h_const_file = fopen(h_const_filename, "w");
  if(!h_const_file) error("Failed to open _const.h output file");

  int symcount;
  struct upb_def **defs = upb_symtab_getdefs(s, &symcount, UPB_DEF_ANY);
  upb_symtab_unref(s);
  write_h(defs, symcount, h_filename, cident, h_file);
  write_const_h(defs, symcount, h_filename, h_const_file);
  for (int i = 0; i < symcount; i++) upb_def_unref(defs[i]);
  free(defs);
  if(cident) {
    FILE *c_file = fopen(c_filename, "w");
    if(!c_file) error("Failed to open .h output file");
    write_message_c(fds_msg, cident, h_filename, argc, argv, input_file, c_file);
    fclose(c_file);
  }
  upb_msg_unref(fds_msg);
  upb_string_unref(descriptor);
  fclose(h_file);
  fclose(h_const_file);

  return 0;
}
