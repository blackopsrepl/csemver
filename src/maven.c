#include "maven.h"

#include "common.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_maven_error(char *error, size_t error_size,
                            const char *message) {
  if (error != NULL && error_size != 0)
    snprintf(error, error_size, "%s", message);
}

static xmlDocPtr parse_document(const char *content, char *error,
                                size_t error_size) {
  size_t length = strlen(content);
  xmlDocPtr document;
  if (length > INT_MAX) {
    set_maven_error(error, error_size, "Maven pom file is too large");
    return NULL;
  }
  document = xmlReadMemory(content, (int)length, NULL, "UTF-8",
                           XML_PARSE_NOBLANKS | XML_PARSE_NONET |
                               XML_PARSE_RECOVER | XML_PARSE_NOERROR |
                               XML_PARSE_NOWARNING | XML_PARSE_NOCDATA);
  if (document == NULL)
    set_maven_error(
        error, error_size,
        "Failed to read the version field in your pom file - is it present?");
  return document;
}

static xmlNodePtr find_child(xmlNodePtr parent, const char *name) {
  xmlNodePtr child;
  if (parent == NULL)
    return NULL;
  for (child = parent->children; child != NULL; child = child->next)
    if (child->type == XML_ELEMENT_NODE &&
        (child->ns == NULL || child->ns->prefix == NULL) &&
        xmlStrEqual(child->name, BAD_CAST name))
      return child;
  return NULL;
}

static size_t count_children(xmlNodePtr parent, const char *name) {
  xmlNodePtr child;
  size_t count = 0;
  if (parent == NULL)
    return 0;
  for (child = parent->children; child != NULL; child = child->next)
    if (child->type == XML_ELEMENT_NODE &&
        (child->ns == NULL || child->ns->prefix == NULL) &&
        xmlStrEqual(child->name, BAD_CAST name))
      ++count;
  return count;
}

static int copy_text(xmlChar *text, char *version, size_t version_size,
                     char *error, size_t error_size) {
  size_t length;
  if (text == NULL || text[0] == '\0') {
    set_maven_error(
        error, error_size,
        "Failed to read the version field in your pom file - is it present?");
    xmlFree(text);
    return 0;
  }
  length = (size_t)xmlStrlen(text);
  if (length >= version_size) {
    set_maven_error(error, error_size, "version output buffer too small");
    xmlFree(text);
    return 0;
  }
  memcpy(version, text, length + 1);
  xmlFree(text);
  return 1;
}

static void trim_xml_text(xmlChar *text) {
  size_t start = 0;
  size_t length;
  if (text == NULL)
    return;
  length = (size_t)xmlStrlen(text);
  while (start < length && (text[start] == ' ' || text[start] == '\t' ||
                            text[start] == '\r' || text[start] == '\n'))
    ++start;
  while (length > start &&
         (text[length - 1] == ' ' || text[length - 1] == '\t' ||
          text[length - 1] == '\r' || text[length - 1] == '\n'))
    --length;
  if (start != 0 || length != (size_t)xmlStrlen(text)) {
    memmove(text, text + start, length - start);
    text[length - start] = '\0';
  }
}

static int select_version(xmlDocPtr document, xmlNodePtr *target,
                          xmlChar **version_text, int allow_empty, char *error,
                          size_t error_size) {
  static const char missing[] =
      "Failed to read the version field in your pom file - is it present?";
  xmlNodePtr project = xmlDocGetRootElement(document);
  xmlNodePtr version_node;
  xmlChar *pom_version;
  const char *property_start;
  const char *property_end;
  char *property_name;
  xmlNodePtr property_node;
  xmlNodePtr properties;
  if (project == NULL || !xmlStrEqual(project->name, BAD_CAST "project") ||
      (project->ns != NULL && project->ns->prefix != NULL)) {
    set_maven_error(
        error, error_size,
        allow_empty ? "Cannot read properties of undefined (reading 'version')"
                    : missing);
    return 0;
  }
  version_node = find_child(project, "version");
  if (version_node != NULL && (version_node->properties != NULL ||
                               count_children(project, "version") > 1)) {
    set_maven_error(error, error_size,
                    "pomVersion.startsWith is not a function");
    return 0;
  }
  pom_version = version_node == NULL ? NULL : xmlNodeGetContent(version_node);
  trim_xml_text(pom_version);
  if (pom_version == NULL || (!allow_empty && pom_version[0] == '\0')) {
    set_maven_error(error, error_size, missing);
    xmlFree(pom_version);
    return 0;
  }
  if (pom_version[0] != '$' || pom_version[1] != '{') {
    *target = version_node;
    *version_text = pom_version;
    return 1;
  }
  property_start = (const char *)pom_version + 2;
  property_end = strchr(property_start, '}');
  if (property_end == NULL || property_end[1] != '\0' ||
      property_end == property_start) {
    set_maven_error(error, error_size,
                    "Failed to read the version field in your pom file - "
                    "unexpected invalid property reference");
    xmlFree(pom_version);
    return 0;
  }
  property_name = malloc((size_t)(property_end - property_start) + 1);
  if (property_name == NULL) {
    set_maven_error(error, error_size, "out of memory");
    xmlFree(pom_version);
    return 0;
  }
  memcpy(property_name, property_start,
         (size_t)(property_end - property_start));
  property_name[property_end - property_start] = '\0';
  properties = find_child(project, "properties");
  property_node = find_child(properties, property_name);
  xmlFree(pom_version);
  pom_version = property_node == NULL ? NULL : xmlNodeGetContent(property_node);
  trim_xml_text(pom_version);
  if (pom_version == NULL || pom_version[0] == '\0') {
    char message[256];
    snprintf(message, sizeof message,
             "Failed to read the %s field in your pom file properties - is it "
             "present?",
             property_name);
    set_maven_error(error, error_size, message);
    xmlFree(pom_version);
    free(property_name);
    return 0;
  }
  free(property_name);
  *target = property_node;
  *version_text = pom_version;
  return 1;
}

int csemver_maven_read_text(const char *content, char *version,
                            size_t version_size, char *error,
                            size_t error_size) {
  xmlDocPtr document = parse_document(content, error, error_size);
  xmlNodePtr target = NULL;
  xmlChar *version_text = NULL;
  int result;
  if (document == NULL)
    return 0;
  result =
      select_version(document, &target, &version_text, 0, error, error_size);
  xmlFreeDoc(document);
  if (!result)
    return 0;
  return copy_text(version_text, version, version_size, error, error_size);
}

static int append_xml_bytes(CsemverBuffer *buffer, const char *text,
                            size_t length, int crlf) {
  size_t i;
  for (i = 0; i < length; ++i) {
    if (text[i] == '\n' && crlf && (i == 0 || text[i - 1] != '\r')) {
      if (!csemver_buffer_append(buffer, "\r\n", 2))
        return 0;
    } else if (!csemver_buffer_append(buffer, text + i, 1)) {
      return 0;
    }
  }
  return 1;
}

static size_t special_xml_end(const char *xml, size_t length, size_t start) {
  const char *ending = NULL;
  size_t ending_length = 0;
  size_t i;
  if (length - start >= 4 && memcmp(xml + start, "<!--", 4) == 0) {
    ending = "-->";
    ending_length = 3;
    i = start + 4;
  } else if (length - start >= 9 && memcmp(xml + start, "<![CDATA[", 9) == 0) {
    ending = "]]>";
    ending_length = 3;
    i = start + 9;
  } else if (length - start >= 2 && xml[start + 1] == '?') {
    ending = "?>";
    ending_length = 2;
    i = start + 2;
  } else {
    char quote = '\0';
    size_t subset_depth = 0;
    for (i = start + 2; i < length; ++i) {
      if (quote != '\0') {
        if (xml[i] == quote)
          quote = '\0';
      } else if (xml[i] == '\'' || xml[i] == '"') {
        quote = xml[i];
      } else if (xml[i] == '[') {
        ++subset_depth;
      } else if (xml[i] == ']' && subset_depth != 0) {
        --subset_depth;
      } else if (xml[i] == '>' && subset_depth == 0) {
        return i + 1;
      }
    }
    return length;
  }
  for (; i + ending_length <= length; ++i)
    if (memcmp(xml + i, ending, ending_length) == 0)
      return i + ending_length;
  return length;
}

static int append_formatted_xml(CsemverBuffer *buffer, const char *xml,
                                size_t length, int crlf) {
  size_t i = 0;
  while (i < length) {
    size_t end;
    size_t before_close;
    size_t name_start;
    size_t name_end;
    char quote = '\0';
    if (xml[i] == '<' && i + 1 < length &&
        (xml[i + 1] == '!' || xml[i + 1] == '?')) {
      end = special_xml_end(xml, length, i);
      if (!append_xml_bytes(buffer, xml + i, end - i, crlf))
        return 0;
      i = end;
      continue;
    }
    if (xml[i] != '<' || i + 1 >= length || xml[i + 1] == '/') {
      if (!append_xml_bytes(buffer, xml + i, 1, crlf))
        return 0;
      ++i;
      continue;
    }
    for (end = i + 1; end < length; ++end) {
      if (quote != '\0') {
        if (xml[end] == quote)
          quote = '\0';
      } else if (xml[end] == '\'' || xml[end] == '"') {
        quote = xml[end];
      } else if (xml[end] == '>') {
        break;
      }
    }
    if (end == length) {
      if (!append_xml_bytes(buffer, xml + i, length - i, crlf))
        return 0;
      break;
    }
    before_close = end;
    while (before_close > i &&
           (xml[before_close - 1] == ' ' || xml[before_close - 1] == '\t' ||
            xml[before_close - 1] == '\r' || xml[before_close - 1] == '\n'))
      --before_close;
    if (before_close > i && xml[before_close - 1] == '/') {
      name_start = i + 1;
      name_end = name_start;
      while (name_end < before_close && xml[name_end] != ' ' &&
             xml[name_end] != '\t' && xml[name_end] != '\r' &&
             xml[name_end] != '\n' && xml[name_end] != '/')
        ++name_end;
      if (name_end == name_start ||
          !append_xml_bytes(buffer, xml + i, before_close - i - 1, crlf) ||
          !append_xml_bytes(buffer, ">", 1, crlf) ||
          !append_xml_bytes(buffer, "</", 2, crlf) ||
          !append_xml_bytes(buffer, xml + name_start, name_end - name_start,
                            crlf) ||
          !append_xml_bytes(buffer, ">", 1, crlf))
        return 0;
    } else if (!append_xml_bytes(buffer, xml + i, end + 1 - i, crlf)) {
      return 0;
    }
    i = end + 1;
  }
  return 1;
}

static int source_uses_crlf(const char *content) {
  const char *cursor;
  for (cursor = content; *cursor != '\0'; ++cursor) {
    if (*cursor == '\n')
      return cursor > content && cursor[-1] == '\r';
    if (*cursor == '\r')
      return cursor[1] == '\n';
  }
  return 0;
}

static int serialize_document(xmlDocPtr document, const char *original,
                              char **updated, size_t *updated_size, char *error,
                              size_t error_size) {
  xmlChar *serialized = NULL;
  int serialized_size = 0;
  const char *body;
  size_t body_size;
  int crlf = source_uses_crlf(original);
  CsemverBuffer buffer;
  xmlDocDumpFormatMemoryEnc(document, &serialized, &serialized_size, "UTF-8",
                            1);
  if (serialized == NULL || serialized_size < 0) {
    xmlFree(serialized);
    set_maven_error(error, error_size,
                    "out of memory serializing Maven pom file");
    return 0;
  }
  body = (const char *)serialized;
  body_size = (size_t)serialized_size;
  if (body_size >= 5 && memcmp(body, "<?xml", 5) == 0) {
    const char *declaration_end = strstr(body, "?>");
    if (declaration_end == NULL) {
      xmlFree(serialized);
      set_maven_error(error, error_size, "unable to serialize Maven pom file");
      return 0;
    }
    body = declaration_end + 2;
    body_size -= (size_t)(body - (const char *)serialized);
    if (body_size != 0 && *body == '\r') {
      ++body;
      --body_size;
    }
    if (body_size != 0 && *body == '\n') {
      ++body;
      --body_size;
    }
  }
  csemver_buffer_init(&buffer);
  if (!append_formatted_xml(&buffer, body, body_size, crlf)) {
    csemver_buffer_free(&buffer);
    xmlFree(serialized);
    set_maven_error(error, error_size,
                    "out of memory serializing Maven pom file");
    return 0;
  }
  if (buffer.length == 0 || buffer.data[buffer.length - 1] != '\n') {
    if (!append_xml_bytes(&buffer, crlf ? "\r\n\r\n" : "\n\n", crlf ? 4 : 2,
                          0)) {
      csemver_buffer_free(&buffer);
      xmlFree(serialized);
      set_maven_error(error, error_size,
                      "out of memory serializing Maven pom file");
      return 0;
    }
  } else if (!append_xml_bytes(&buffer, crlf ? "\r\n" : "\n", crlf ? 2 : 1,
                               0)) {
    csemver_buffer_free(&buffer);
    xmlFree(serialized);
    set_maven_error(error, error_size,
                    "out of memory serializing Maven pom file");
    return 0;
  }
  xmlFree(serialized);
  *updated = buffer.data;
  *updated_size = buffer.length;
  return 1;
}

int csemver_maven_update_text(const char *content, const char *new_version,
                              char **updated, size_t *updated_size,
                              char *old_version, size_t old_version_size,
                              char *error, size_t error_size) {
  xmlDocPtr document = parse_document(content, error, error_size);
  xmlNodePtr target = NULL;
  xmlChar *version_text = NULL;
  size_t version_length;
  if (document == NULL)
    return 0;
  if (!select_version(document, &target, &version_text, 1, error, error_size)) {
    xmlFreeDoc(document);
    return 0;
  }
  version_length = (size_t)xmlStrlen(version_text);
  if (old_version == NULL || version_length >= old_version_size) {
    set_maven_error(error, error_size, "version output buffer too small");
    xmlFree(version_text);
    xmlFreeDoc(document);
    return 0;
  }
  memcpy(old_version, version_text, version_length + 1);
  xmlFree(version_text);
  xmlNodeSetContent(target, BAD_CAST new_version);
  if (!serialize_document(document, content, updated, updated_size, error,
                          error_size)) {
    xmlFreeDoc(document);
    return 0;
  }
  xmlFreeDoc(document);
  return 1;
}
