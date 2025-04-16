/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "config.h"

#include <assert.h>
#include <getopt.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include <systemd/sd-json.h>

#include "basics.h"
#include "extension-util.h"
#include "sysext-cli.h"
#include "osrelease.h"
#include "extrelease.h"
#include "download.h"
#include "extract.h"
#include "tmpfile-util.h"
#include "strv.h"
#include "images-list.h"

static int
readlink_malloc(const char *path, const char *name, char **ret)
{
  _cleanup_free_ char *fn = NULL;
  _cleanup_free_ char *buf = NULL;
  ssize_t nbytes, bufsiz;
  struct stat sb;
  int r;

  r = join_path(path, name, &fn);
  if (r < 0)
    return r;

  if (lstat(fn, &sb) == -1)
    {
      perror("lstat");
      return -errno;
    }

  /* Add one to the link size, so that we can determine whether
     the buffer returned by readlink() was truncated. */
  bufsiz = sb.st_size + 1;

  /* Some magic symlinks under (for example) /proc and /sys
     report 'st_size' as zero. In that case, take PATH_MAX as
     a "good enough" estimate. */
  if (sb.st_size == 0)
    bufsiz = PATH_MAX;

  buf = malloc(bufsiz);
  if (buf == NULL)
    return -ENOMEM;

  nbytes = readlink(fn, buf, bufsiz);
  if (nbytes == -1)
    return -errno;

  if (nbytes == bufsiz)
    {
      fprintf(stderr, "Returned buffer may have been truncated\n");
      exit(EXIT_FAILURE);
    }

  /* It doesn't contain a terminating null byte ('\0'). */
  buf[nbytes] = '\0';

  *ret = TAKE_PTR(buf);

  return 0;
}

static int
image_filter(const struct dirent *de)
{
  if (endswith(de->d_name, ".raw") || endswith(de->d_name, ".img"))
    return 1;
  return 0;
}

int
discover_images(const char *path, char ***result)
{
  struct dirent **de = NULL;
  int r;

  assert(result);

  int num_dirs = scandir(path, &de, image_filter, alphasort);
  if (num_dirs < 0)
    return -errno;

  if (num_dirs > 0)
    {
      *result = malloc((num_dirs+1) * sizeof(char *));
      if (*result == NULL)
	oom();
      (*result)[num_dirs] = NULL;

      for (int i = 0; i < num_dirs; i++)
      {
	if (de[i]->d_type == DT_LNK)
	  {
	    _cleanup_free_ char *fn = NULL;
	    char *p;

	    r = readlink_malloc(path, de[i]->d_name, &fn);
	    if (r < 0)
	      return r;

	    p = strrchr(fn, '/');
	    if (p)
	      (*result)[i] = strdup(++p);
	    else
	      (*result)[i] = strdup(fn);
	  }
	else
	  (*result)[i] = strdup(de[i]->d_name);

	if ((*result)[i] == NULL)
	  oom();
	free(de[i]);
      }
      free(de);
    }

  return 0;
}

static int
image_read_metadata(const char *image_name, struct image_deps **res)
{
  _cleanup_(free_image_depsp) struct image_deps *image = NULL;
  _cleanup_(unlink_tempfilep) char tmpfn[] = "/tmp/sysext-image-extrelease.XXXXXX";
  _cleanup_close_ int fd = -EBADF;
  int r;

  assert(image_name);
  assert(res);

  fd = mkostemp_safe(tmpfn);

  r = extract(SYSEXT_STORE_DIR, image_name, fd);
  if (r < 0)
    {
      fprintf(stderr, "Failed to extract extension-release from '%s': %s\n",
	      image_name, strerror(-r));
      return r;
    }
  else if (r > 0)
    {
      fprintf(stderr, "Failed to extract extension-release from '%s': systemd-dissect failed (%i)\n",
	      image_name, r);
      return -EINVAL;
    }

  r = load_ext_release(image_name, tmpfn, &image);
  if (r < 0)
    return r;

  if (image)
    *res = TAKE_PTR(image);

  return 0;
}

static int
image_json_from_url(const char *url, const char *image_name, struct image_deps **res)
{
  _cleanup_(unlink_tempfilep) char tmpfn[] = "/tmp/sysext-image-json.XXXXXX";
  _cleanup_close_ int fd = -EBADF;
  _cleanup_free_ char *jsonfn = NULL;
  int r;

  assert(url);
  assert(res);

  fd = mkostemp_safe(tmpfn);

  jsonfn = malloc(strlen(image_name) + strlen(".json") + 1);
  char *p = stpcpy(jsonfn, image_name);
  strcpy(p, ".json");

  r = download(url, jsonfn, tmpfn, false /*XXX*/);
  if (r < 0)
    {
      fprintf(stderr, "Failed to download '%s' from '%s': %s",
	      jsonfn, url, strerror(-r));
      return r;
    }

  _cleanup_(free_image_deps_list) struct image_deps **images = NULL;
  r = load_image_json(fd, tmpfn, &images);
  if (r < 0)
    return r;

  if (images == NULL || images[0] == NULL)
    {
      fprintf(stderr, "No entry with dependencies found (%s)!\n", jsonfn);
      return -ENOENT;
    }

  if (images[1] == NULL)
      *res = TAKE_PTR(images[0]);
  else
    {
      /* XXX go through the list and search the corret image */
      /* XXX we cannot use TAKE_PTR else the rest of the list will not be free'd */
      fprintf(stderr, "More than one entry found, not implemented yet!\n");
      exit(EXIT_FAILURE);
    }

  return 0;
}

static int
image_list_from_url(const char *url, char ***result)
{
  _cleanup_(unlink_tempfilep) char tmpfn[] = "/tmp/sysext-SHA256SUMS.XXXXXX";
  _cleanup_close_ int fd = -EBADF;
  _cleanup_fclose_ FILE *fp = NULL;
  int r;

  assert(url);
  assert(result);

  fd = mkostemp_safe(tmpfn);

  r = download(url, "SHA256SUMS", tmpfn, false /*XXX*/);
  if (r < 0)
    {
      fprintf(stderr, "Failed to download 'SHA256SUMS' from '%s': %s",
	      url, strerror(-r));
      return r;
    }

  fp = fdopen(fd, "r");
  if (!fp)
    return -errno;

  size_t cur_entry = 0, max_entry = 10;
  *result = malloc((max_entry + 1) * sizeof(char *));
  if (*result == NULL)
    oom();
  (*result)[0] = NULL;

  _cleanup_(freep) char *line = NULL;
  size_t size = 0;
  ssize_t nread;

  while ((nread = getline(&line, &size, fp)) != -1)
    {
      /* Remove trailing newline character */
      if (nread && line[nread-1] == '\n')
	line[nread-1] = '\0';

      if (endswith(line, ".raw") || endswith(line, ".img"))
	{
	  /* get image name, skip SHA256SUM hash and spaces */
	  char *p = strchr(line, ' ');
	  while (*p == ' ')
	    ++p;

	  if (cur_entry == max_entry)
	    {
	      max_entry = max_entry * 2;
	      *result = realloc(*result, (max_entry + 1) * sizeof(char *));
	      if (*result == NULL)
		oom();
	    }
	  (*result)[cur_entry] = strdup(p);
	  if ((*result)[cur_entry] == NULL)
	    oom();
	  cur_entry++;
	  (*result)[cur_entry] = NULL;
	}
    }

  return 0;
}

int
image_remote_metadata(const char *url, struct image_entry ***res, size_t *nr)
{
  _cleanup_strv_free_ char **list = NULL;
  _cleanup_(free_image_entry_list) struct image_entry **images = NULL;
  size_t n = 0;
  int r;

  assert(url);
  assert(res);

  r = image_list_from_url(url, &list);
  if (r < 0)
    return -r;

  n = strv_length(list);
  if (n > 0)
    {
      images = malloc((n+1) * sizeof(struct image_entry *));
      if (images == NULL)
	return -ENOMEM;
      images[n] = NULL;

      for (size_t i = 0; i < n; i++)
	{
	  char *p;

	  images[i] = calloc(1, sizeof(struct image_entry));
	  if (images[i] == NULL)
	    return -ENOMEM;
	  images[i]->name = strdup(list[i]);
	  if (images[i]->name == NULL)
	    return -ENOMEM;
	  images[i]->remote = true;

	  /* create "debug-tools" from "debug-tools-23.7.x86-64.raw" */
	  p = strrchr(images[i]->name, '.'); /* raw */
	  if (p)
	    *p = '\0';
	  p = strrchr(images[i]->name, '.'); /* arch */
	  if (p)
	    *p = '\0';
	  p = strrchr(images[i]->name, '-'); /* version */
	  if (p)
	    *p = '\0';

	  r = image_json_from_url(url, list[i], &(images[i]->deps));
	  if (r < 0)
	    return -r;
	}
    }

  if (nr)
    *nr = n;
  if (images)
    *res = TAKE_PTR(images);

  return 0;
}

int
image_local_metadata(const char *store, struct image_entry ***res, size_t *nr)
{
  _cleanup_strv_free_ char **list = NULL;
  _cleanup_(free_image_entry_list) struct image_entry **images = NULL;
  size_t n = 0;
  int r;

  r = discover_images(store, &list);
  if (r < 0)
    {
      fprintf(stderr, "Scan local images failed: %s\n", strerror(-r));
      return -r;
    }

  n = strv_length(list);
  if (n > 0)
    {
      images = malloc((n+1) * sizeof(struct image_entry *));
      if (images == NULL)
	return -ENOMEM;
      images[n] = NULL;

      for (size_t i = 0; i < n; i++)
	{
	  char *p;

	  images[i] = calloc(1, sizeof(struct image_entry));
	  if (images[i] == NULL)
	    return -ENOMEM;
	  images[i]->name = strdup(list[i]);
	  if (images[i]->name == NULL)
	    return -ENOMEM;

	  /* create "debug-tools" from "debug-tools-23.7.x86-64.raw" */
	  p = strrchr(images[i]->name, '.'); /* raw */
	  if (p)
	    *p = '\0';
	  p = strrchr(images[i]->name, '.'); /* arch */
	  if (p)
	    *p = '\0';
	  p = strrchr(images[i]->name, '-'); /* version */
	  if (p)
	    *p = '\0';

	  images[i]->local = true;

	  r = image_read_metadata(list[i], &(images[i]->deps));
	  if (r < 0)
	    return r;
	}
    }

  if (nr)
    *nr = n;
  if (images)
    *res = TAKE_PTR(images);

  return 0;
}
