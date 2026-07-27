/*
 * nsxml.h -- just enough XML for WebDAV multistatus responses.
 *
 * SAX style: the caller feeds the whole document and gets callbacks per
 * element. Namespace prefixes are stripped ("d:href" -> "href"). Entities
 * &amp; &lt; &gt; &quot; &apos; and numeric &#nn; are decoded in text.
 */

#ifndef NSXML_H
#define NSXML_H

typedef struct nsxml_parser nsxml_parser;

typedef void (*nsxml_start_fn)(void *user, const char *tag);
typedef void (*nsxml_end_fn)(void *user, const char *tag, const char *text);

/*
 * Parse doc (len bytes). start is called on every element open, end on
 * every close with the accumulated character data of that element (leaf
 * text only, trimmed). Returns 0 on success, -1 on malformed input.
 */
int nsxml_parse(const char *doc, long len,
                nsxml_start_fn start, nsxml_end_fn end, void *user);

#endif
