/*
 *  Blacklist based file access control, unit tests
 *
 *  Copyright (C) 2015 Jolla Ltd.
 *  Contact: Hannu Mallat <hannu.mallat@jollamobile.com>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "plugins/jolla-blacklist.c"
#include <sys/stat.h>

static void test_path_normalization(void)
{
	static const char *abs_input[] = {
		"/",
		"///",

		"/dir1",
		"///dir1",
		"/dir1/",

		"/dir1/dir2",
		"/dir1///dir2",
		"/dir1/dir2/",

		"/dir1/./dir2/././dir3",
		"/dir1/dir2/.",
		"/dir1/dir2/./",

		"/dir1/dir2/..",
		"/dir1/dir2/../",

		"/dir1/dir2/dir3/../dir4",
		"/dir1/dir2/dir3/../dir4/",

		"/dir1/dir2/dir3/../../../../../dir4",
		"/dir1/dir2/dir3/../../../../../dir4/",
		"/../../dir1",

		NULL
	};
	static const char *abs_expected[] = {
		"/",
		"/",

		"/dir1",
		"/dir1",
		"/dir1",

		"/dir1/dir2",
		"/dir1/dir2",
		"/dir1/dir2",

		"/dir1/dir2/dir3",
		"/dir1/dir2",
		"/dir1/dir2",

		"/dir1",
		"/dir1",

		"/dir1/dir2/dir4",
		"/dir1/dir2/dir4",

		"/dir4",
		"/dir4",
		"/dir1",

		NULL
	};
	int i;

	g_assert(normalized_path(NULL) == NULL);
	g_assert(normalized_path("not/absolute") == NULL);

	for (i = 0; abs_input[i]; i++) {
		gchar *result = normalized_path(abs_input[i]);
		g_assert_cmpstr(result, ==, abs_expected[i]);
		g_free(result);
	}

}

const gchar *simple_blacklist =
	".ssh\n"
	".invisible_file\n"
	"Music/DRM\n"
	;

const gchar *trim_blacklist =
	"\t.ssh\n"
	"        .invisible_file        \n"
	"Music/DRM\t\n"
	"\n"
	"         \n"
	"\t"
	;

const gchar *comment_blacklist =
	"# this is a test\n"
	".ssh\n"
	".invisible_file\n"
	"# why even have this?\n"
	"Music/DRM\n"
	"# close to the end\n"
	;

static void test_blacklist_reading_valid_blacklists(void)
{
	const gchar *test_blacklists[] = {
		simple_blacklist,
		trim_blacklist,
		comment_blacklist,
		NULL
	};

	int i;

	blacklist_clear();

	for (i = 0; test_blacklists[i]; i++) {
		struct blacklist_data *data;
		char tmplate[] = "/tmp/test-jolla-blacklist.XXXXXX";
		int fd = mkstemp(tmplate);
		g_assert_cmpint(fd, >=, 0);
		g_assert(g_file_set_contents(tmplate,
						test_blacklists[i],
						strlen(test_blacklists[i]),
						NULL) == TRUE);
		g_assert(blacklist_add("/home/nemo", tmplate) == 0);
		
		/* Check data is as expected */
		g_assert_cmpint(g_slist_length(blacklists), ==, 1);
		data = blacklists->data;
		g_assert_cmpstr(data->path, ==, "/home/nemo");
		g_assert_cmpint(data->elem->len, ==, 3);
		g_assert_cmpstr(data->elem->pdata[0], ==, ".ssh");
		g_assert_cmpstr(data->elem->pdata[1], ==, ".invisible_file");
		g_assert_cmpstr(data->elem->pdata[2], ==, "Music/DRM");

		close(fd);
		unlink(tmplate);

		blacklist_clear();
	}
}

const gchar *home_nemo_blacklist =
	".ssh\n"
	".invisible_file\n"
	"Music/DRM\n"
	;

const gchar *home_nemo_Documents_blacklist =
	"Mailbox\n"
	"Work/Restricted\n"
	;

const gchar *sdcard_blacklist =
	"Music/DRM\n"
	;

static void test_blacklist_matching(void)
{
	const gchar *test_blacklists[] = {
		home_nemo_blacklist,
		home_nemo_Documents_blacklist,
		sdcard_blacklist,
		NULL
	};
	const gchar *test_blacklist_roots[] = {
		"/home/nemo",
		"/home/nemo/Documents",
		"/media/sdcard",
		NULL
	};
	const gchar *matching_paths[] = {
		"/home/nemo/.ssh",
		"/home/nemo/.ssh/",
		"/home/nemo/.ssh/./",
		"/home/nemo/../nemo/.ssh",
		"/home/nemo/.invisible_file",
		"/home/nemo/Music/DRM/BoringArtist/BoringAlbum",
		"/home/nemo/Documents/Mailbox/John_Doe",
		"/home/nemo/Documents/Work/Restricted/schedule.ppt",
		"/media/sdcard/Music/DRM/BoringArtist/BoringAlbum",

		NULL
	};
	const gchar *non_matching_paths[] = {
		"/home/nemo",
		"/home/nemo/.invisible_file2",
		"/home/nemo/Documents",
		"/home/nemo/Documents/Shared",
		"/home/nemo/Music",
		"/home/nemo/Music/GoodArtist",
		"/home",
		"/usr",
		"/",
		"/media/sdcard/Music/GoodArtist",

		NULL
	};
	int i;

	blacklist_clear();

	for (i = 0; test_blacklists[i]; i++) {
		char tmplate[] = "/tmp/test-jolla-blacklist.XXXXXX";
		int fd = mkstemp(tmplate);
		g_assert_cmpint(fd, >=, 0);
		g_assert(g_file_set_contents(tmplate,
						test_blacklists[i],
						strlen(test_blacklists[i]),
						NULL) == TRUE);
		g_assert(blacklist_add(test_blacklist_roots[i], tmplate) == 0);

		close(fd);
		unlink(tmplate);
	}

	for (i = 0; matching_paths[i]; i++)
		g_assert(blacklist_match(matching_paths[i]) == TRUE);

	for (i = 0; non_matching_paths[i]; i++)
		g_assert(blacklist_match(non_matching_paths[i]) == FALSE);

	g_assert(blacklist_match(NULL) == FALSE);
	g_assert(blacklist_match("not/absolute/path") == FALSE);

	blacklist_clear();
}

static gchar *setup_config_dir(const gchar *xml_fmt,
			const char *list_name,
			const char *list_data)
{
	char tmpdir[] = "/tmp/test-jolla-blacklist-config.XXXXXX";
	gchar *fname = NULL;
	gchar *buf = NULL;

	g_assert(mkdtemp(tmpdir) != NULL);

	fname = g_build_filename(tmpdir, "test.xml", NULL);
	buf = g_strdup_printf(xml_fmt, tmpdir);
	DBG("%s", buf);
	g_assert(g_file_set_contents(fname, buf, strlen(buf), NULL) == TRUE);
	g_free(buf);
	g_free(fname);
	fname = g_build_filename(tmpdir, list_name, NULL);
	g_assert(g_file_set_contents(fname, list_data,
					strlen(list_data), NULL) == TRUE);
	g_free(fname);

	return g_strdup(tmpdir);
}

static void cleanup_config_dir(gchar *dirname)
{
	const gchar *name;
	GDir *dir;

	dir = g_dir_open(dirname, 0, NULL);
	while ((name = g_dir_read_name(dir)) != NULL) {
		gchar *fullname = g_build_filename(dirname, name, NULL);
		unlink(fullname);
		g_free(fullname);
	}
	g_dir_close(dir);
	rmdir(dirname);
	g_free(dirname);
}

static void test_blacklist_reading_valid_xml(void)
{
}

static void test_blacklist_reading_invalid_xml(void)
{
	static const gchar *bad_xml[] = {
		/* Relative storage path */
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage path=\"not/absolute\" name=\"media\">\n"
		"  <blacklist>%s/test.conf</blacklist>\n"
		"</storage>\n",

		/* Missing storage path */
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage name=\"media\">\n"
		"  <blacklist>%s/test.conf</blacklist>\n"
		"</storage>\n",

		/* Duplicate storage path */
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage path=\"/home/nemo\" path=\"/home/nemo\">\n"
		"  <blacklist>%s/test.conf</blacklist>\n"
		"</storage>\n",

		/* Unknown storage attribute */
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage name=\"Test\" path=\"/home/nemo\" foo=\"bar\">\n"
		"  <blacklist>%s/test.conf</blacklist>\n"
		"</storage>\n",

		/* Unknown blacklist attribute */
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage name=\"Test\" path=\"/home/nemo\">\n"
		"  <blacklist foo=\"bar\">%s/test.conf</blacklist>\n"
		"</storage>\n",

		/* Missing blacklist data */
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage name=\"Test\" path=\"/home/nemo\">\n"
		"  <blacklist></blacklist>\n"
		"</storage>\n",

		/* Missing blacklist data (all whitespace) */
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage name=\"Test\" path=\"/home/nemo\">\n"
		"  <blacklist>        </blacklist>\n"
		"</storage>\n",

		/* Unknown element */
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage name=\"Test\" path=\"/home/nemo\">\n"
		"  <blacklist>%s/test.conf</blacklist>\n"
		"  <whitelist>foo.conf</whitelist>\n"
		"</storage>\n",

		/* Bad nesting */
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage name=\"Test\" path=\"/home/nemo\">\n"
		"  <storage name=\"Test\" path=\"/home/nemo\">\n"
		"    <blacklist>%s/test.conf</blacklist>\n"
		"  </storage>\n"
		"</storage>\n",

		/* Unexpected text */
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage name=\"Test\" path=\"/home/nemo\">\n"
		"  dummy\n"
		"  <blacklist>%s/test.conf</blacklist>\n"
		"</storage>\n",

		NULL
	};
	static const gchar *blacklist =
		".foo\n"
		".bar\n"
		".baz\n"
		;
	static const gchar *blackfile = "test.conf";
	int i;

	blacklist_clear();

	for (i = 0; bad_xml[i]; i++) {
		gchar *dirname = NULL;
		gchar *pathname = NULL;
		dirname = setup_config_dir(bad_xml[i], blackfile, blacklist);
		config_dir = dirname;
		pathname = g_build_filename(dirname, "test.xml", NULL);
		g_assert_cmpint(append_config(pathname), !=, 0);
		g_assert(blacklists == NULL);
		g_free(pathname);
		cleanup_config_dir(dirname);
	}
}

static void test_init_filled(void)
{
	static const gchar *test_xml_fmt =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<storage name=\"media\" path=\"/home/nemo\"\n"
		"         description=\"Phone Memory\">\n"
		"<blacklist>%s/blacklist-home.conf</blacklist>\n"
		"</storage>\n"
		;
	static const gchar *test_blacklist =
		".foo\n"
		".bar\n"
		".baz\n"
		;
	static const gchar *test_blackfile = "blacklist-home.conf";
	gchar *dirname = NULL;

	dirname = setup_config_dir(test_xml_fmt, test_blackfile, test_blacklist);
	config_dir = dirname;
	g_assert_cmpint(jolla_blacklist_init(), ==, 0);

	/* Check that plugin gets called */
	g_assert_cmpint(access_check(NULL, 0, ACCESS_OP_LIST, "/home/nemo/.foo"),
			!=, 0);
	g_assert_cmpint(access_check(NULL, 0, ACCESS_OP_LIST, "/home/nemo/.xyz"),
			==, 0);
	g_assert_cmpint(access_check_at(NULL, 0, ACCESS_OP_LIST, "/home/nemo", ".foo"),
			!=, 0);
	g_assert_cmpint(access_check_at(NULL, 0, ACCESS_OP_LIST, "/home/nemo", ".xyz"),
			==, 0);

	jolla_blacklist_exit();
	cleanup_config_dir(dirname);
}

static void test_init_empty(void)
{
	char tmpdir[] = "/tmp/test-jolla-blacklist-config.XXXXXX";

	g_assert(mkdtemp(tmpdir) != NULL);
	config_dir = tmpdir;
	g_assert_cmpint(jolla_blacklist_init(), ==, 0);
	jolla_blacklist_exit();
	unlink(tmpdir);
}

static void test_init_nonexistent(void)
{
	g_assert_cmpint(jolla_blacklist_init(), !=, 0);
}

static void test_init_error(void)
{
	char tmpdir[] = "/tmp/test-jolla-blacklist-config.XXXXXX";

	g_assert(mkdtemp(tmpdir) != NULL);
	config_dir = tmpdir;
	chmod(tmpdir, 0220); /* Remove r and x to trigger error */
	g_assert_cmpint(jolla_blacklist_init(), !=, 0);
	unlink(tmpdir);
}

static void test_init_double(void)
{
	char tmpdir[] = "/tmp/test-jolla-blacklist-config.XXXXXX";

	g_assert(mkdtemp(tmpdir) != NULL);
	config_dir = tmpdir;
	g_assert_cmpint(jolla_blacklist_init(), ==, 0);
	g_assert_cmpint(jolla_blacklist_init(), !=, 0);
	jolla_blacklist_exit();
	unlink(tmpdir);
}

#define DUMMY_USER_DATA 12345

static int dummy_access_check(const uint8_t *target,
				gsize target_len,
				enum access_op op,
				const gchar *object,
				gpointer user_data)
{
	return GPOINTER_TO_INT(user_data) == DUMMY_USER_DATA ? 0 : -EPERM;
}

static int dummy_access_check_at(const uint8_t *target,
				gsize target_len,
				enum access_op op,
				const gchar *parent,
				const gchar *object,
				gpointer user_data)
{
	return GPOINTER_TO_INT(user_data) == DUMMY_USER_DATA ? 0 : -EPERM;
}

static void test_access_plugin_reg(void)
{
	struct access_plugin p = {
		dummy_access_check, dummy_access_check_at
	};
	struct access_plugin b1 = {
		NULL, dummy_access_check_at
	};
	struct access_plugin b2 = {
		dummy_access_check, NULL
	};
	struct access_plugin b3 = {
		NULL, NULL
	};

	/* Normal case */
	g_assert_cmpint(access_plugin_register("dummy", &p, NULL), ==, 0);
	g_assert_cmpint(access_plugin_unregister("dummy"), ==, 0);

	/* Bad param reg must fail */
	g_assert_cmpint(access_plugin_register("dummy", NULL, NULL), !=, 0);
	g_assert_cmpint(access_plugin_register(NULL, &p, NULL), !=, 0);
	g_assert_cmpint(access_plugin_register(NULL, NULL, NULL), !=, 0);
	g_assert_cmpint(access_plugin_register("dummy", &b1, NULL), !=, 0);
	g_assert_cmpint(access_plugin_register("dummy", &b2, NULL), !=, 0);
	g_assert_cmpint(access_plugin_register("dummy", &b3, NULL), !=, 0);

	/* Double reg must fail */
	g_assert_cmpint(access_plugin_register("dummy", &p, NULL), ==, 0);
	g_assert_cmpint(access_plugin_register("dummy2", &p, NULL), !=, 0);
	g_assert_cmpint(access_plugin_unregister("dummy"), ==, 0);

	/* Wrong unreg must fail */
	g_assert_cmpint(access_plugin_register("dummy", &p, NULL), ==, 0);
	g_assert_cmpint(access_plugin_unregister("nondummy"), !=, 0);
	g_assert_cmpint(access_plugin_unregister("dummy"), ==, 0);

	/* Nonexistent unreg must fail */
	g_assert_cmpint(access_plugin_unregister("dummy"), !=, 0);
}

static void test_access_plugin_check(void)
{
	struct access_plugin p = {
		dummy_access_check, dummy_access_check_at
	};
	enum access_op op[] = {
		ACCESS_OP_LIST,
		ACCESS_OP_READ,
		ACCESS_OP_WRITE,
		ACCESS_OP_CREATE,
		ACCESS_OP_DELETE
	};
	int i;

	/* Checks without plugin must succeed */
	for (i = 0; i < 5; i++)
		g_assert_cmpint(access_check(NULL, 0, op[i], "/home/nemo/foo"),
				==,
				0);
	for (i = 0; i < 5; i++)
		g_assert_cmpint(access_check_at(NULL, 0, op[i], "/home",
								"nemo/foo"),
				==,
				0);

	/* Checks with plugin must go through to the plugin functions */
	g_assert_cmpint(access_plugin_register("dummy", &p, GINT_TO_POINTER(DUMMY_USER_DATA)), ==, 0);
	g_assert_cmpint(access_check(NULL, 0, op[i], "/home/nemo/foo"), ==, 0);
	g_assert_cmpint(access_check_at(NULL, 0, op[i], "/home", "nemo/foo"),
				==,
				0);
	g_assert_cmpint(access_plugin_unregister("dummy"), ==, 0);
}

int main(int argc, char *argv[])
{
	__obex_log_init(argv[0], "-d", FALSE);
	__obex_log_enable_debug();

	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/access/plugin/register", test_access_plugin_reg);
	g_test_add_func("/access/plugin/check", test_access_plugin_check);

	g_test_add_func("/blacklist/init/empty", test_init_empty);
	g_test_add_func("/blacklist/init/nonexistent", test_init_nonexistent);
	g_test_add_func("/blacklist/init/filled", test_init_filled);
	g_test_add_func("/blacklist/init/error", test_init_error);
	g_test_add_func("/blacklist/init/double", test_init_double);

	g_test_add_func("/blacklist/filenames/test_path_normalization",
			test_path_normalization);

	g_test_add_func("/blacklist/datafiles/test_reading_valid_blacklists",
			test_blacklist_reading_valid_blacklists);

	g_test_add_func("/blacklist/xmlfiles/test_reading_valid_xml",
			test_blacklist_reading_valid_xml);
	g_test_add_func("/blacklist/xmlfiles/test_reading_invalid_xml",
			test_blacklist_reading_invalid_xml);

	g_test_add_func("/blacklist/matching/test_matches",
			test_blacklist_matching);

	g_test_run();

	return 0;
}
