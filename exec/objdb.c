/*
 * Copyright (c) 2006 MontaVista Software, Inc.
 * Copyright (c) 2007-2008 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Steven Dake (sdake@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <errno.h>
#include "objdb.h"
#include "config.h"
#include "main.h"
#include "../lcr/lcr_comp.h"
#include "../include/hdb.h"
#include "../include/list.h"

struct object_key {
	void *key_name;
	int key_len;
	void *value;
	int value_len;
	struct list_head list;
};

struct object_tracker {
	unsigned int object_handle;
	void * data_pt;
	object_track_depth_t depth;
	object_key_change_notify_fn_t key_change_notify_fn;
	object_create_notify_fn_t object_create_notify_fn;
	object_destroy_notify_fn_t object_destroy_notify_fn;
	struct list_head tracker_list;
	struct list_head object_list;
};

struct object_instance {
	void *object_name;
	int object_name_len;
	unsigned int object_handle;
	unsigned int parent_handle;
	struct list_head key_head;
	struct list_head child_head;
	struct list_head child_list;
	struct list_head *find_child_list;
	struct list_head *iter_key_list;
	struct list_head *iter_list;
	void *priv;
	struct object_valid *object_valid_list;
	int object_valid_list_entries;
	struct object_key_valid *object_key_valid_list;
	int object_key_valid_list_entries;
	struct list_head track_head;
};

struct object_find_instance {
	struct list_head *find_child_list;
	struct list_head *child_head;
	void *object_name;
	int object_len;
};

struct objdb_iface_ver0 objdb_iface;
struct list_head objdb_trackers_head;

static struct hdb_handle_database object_instance_database = {
	.handle_count	= 0,
	.handles	= 0,
	.iterator	= 0,
	.mutex		= PTHREAD_MUTEX_INITIALIZER
};

static struct hdb_handle_database object_find_instance_database = {
	.handle_count	= 0,
	.handles	= 0,
	.iterator	= 0,
	.mutex		= PTHREAD_MUTEX_INITIALIZER
};


static int objdb_init (void)
{
	unsigned int handle;
	struct object_instance *instance;
	unsigned int res;

	res = hdb_handle_create (&object_instance_database,
		sizeof (struct object_instance), &handle);
	if (res != 0) {
		goto error_exit;
	}
	res = hdb_handle_get (&object_instance_database,
		handle, (void *)&instance);
	if (res != 0) {
		goto error_destroy;
	}
	instance->find_child_list = &instance->child_head;
	instance->object_name = "parent";
	instance->object_name_len = strlen ("parent");
	instance->object_handle = handle;
	instance->priv = NULL;
	instance->object_valid_list = NULL;
	instance->object_valid_list_entries = 0;
	list_init (&instance->key_head);
	list_init (&instance->child_head);
	list_init (&instance->child_list);
	list_init (&instance->track_head);
	list_init (&objdb_trackers_head);

	hdb_handle_put (&object_instance_database, handle);
	return (0);

error_destroy:
	hdb_handle_destroy (&object_instance_database, handle);

error_exit:
	return (-1);
}

static int _object_notify_deleted_children(struct object_instance *parent_pt)
{
	struct list_head *list;
	struct list_head *notify_list;
	int res;
	struct object_instance *obj_pt = NULL;
	struct object_tracker * tracker_pt;

	for (list = parent_pt->child_head.next;
		 list != &parent_pt->child_head; list = list->next) {

		obj_pt = list_entry(list, struct object_instance,
							child_list);
		res = _object_notify_deleted_children(obj_pt);
		if (res)
			return res;

		for (notify_list = obj_pt->track_head.next;
			 notify_list != &obj_pt->track_head;
			 notify_list = notify_list->next) {

			tracker_pt = list_entry (notify_list, struct object_tracker, object_list);

			if ((tracker_pt != NULL) &&
				(tracker_pt->object_destroy_notify_fn != NULL))
				tracker_pt->object_destroy_notify_fn(parent_pt->object_handle,
													 obj_pt->object_name,
													 obj_pt->object_name_len,
													 tracker_pt->data_pt);
		}
	}

	return 0;
}

static void object_created_notification(unsigned int object_handle,
										unsigned int parent_object_handle,
										void *name_pt, int name_len)
{
	struct list_head * list;
	struct object_instance * obj_pt;
	struct object_tracker * tracker_pt;
	unsigned int obj_handle = object_handle;
	unsigned int res;

	do {
		res = hdb_handle_get (&object_instance_database,
							  obj_handle, (void *)&obj_pt);

		for (list = obj_pt->track_head.next;
			 list != &obj_pt->track_head; list = list->next) {

			tracker_pt = list_entry (list, struct object_tracker, object_list);

			if (((obj_handle == parent_object_handle) ||
				 (tracker_pt->depth == OBJECT_TRACK_DEPTH_RECURSIVE)) &&
				(tracker_pt->object_create_notify_fn != NULL)) {
				tracker_pt->object_create_notify_fn(object_handle, parent_object_handle,
									 name_pt, name_len,
									 tracker_pt->data_pt);
			}
		}

		hdb_handle_put (&object_instance_database, obj_handle);
		obj_handle = obj_pt->parent_handle;

	} while (obj_pt->object_handle != OBJECT_PARENT_HANDLE);

}

static void object_pre_deletion_notification(unsigned int object_handle,
											 unsigned int parent_object_handle,
											 void *name_pt, int name_len)
{
	struct list_head * list;
	struct object_instance * obj_pt;
	struct object_tracker * tracker_pt;
	unsigned int obj_handle = object_handle;
	unsigned int res;

	do {
		res = hdb_handle_get (&object_instance_database,
							  obj_handle, (void *)&obj_pt);

		for (list = obj_pt->track_head.next;
			 list != &obj_pt->track_head; list = list->next) {

			tracker_pt = list_entry (list, struct object_tracker, object_list);

			if (((obj_handle == parent_object_handle) ||
				 (tracker_pt->depth == OBJECT_TRACK_DEPTH_RECURSIVE)) &&
				(tracker_pt->object_destroy_notify_fn != NULL)) {
				tracker_pt->object_destroy_notify_fn(parent_object_handle,
									 name_pt, name_len,
									 tracker_pt->data_pt);
			}
		}
		/* notify child object listeners */
		if (obj_handle == object_handle)
			_object_notify_deleted_children(obj_pt);

		hdb_handle_put (&object_instance_database, obj_handle);
		obj_handle = obj_pt->parent_handle;

	} while (obj_pt->object_handle != OBJECT_PARENT_HANDLE);

}

static void object_key_changed_notification(unsigned int object_handle,
											void *name_pt,	int name_len,
											void *value_pt, int value_len,
											object_change_type_t type)
{
	struct list_head * list;
	struct object_instance * obj_pt;
	struct object_instance * owner_pt = NULL;
	struct object_tracker * tracker_pt;
	unsigned int obj_handle = object_handle;
	unsigned int res;

	do {
		res = hdb_handle_get (&object_instance_database,
							  obj_handle, (void *)&obj_pt);
		if (owner_pt == NULL)
			owner_pt = obj_pt;

		for (list = obj_pt->track_head.next;
			 list != &obj_pt->track_head; list = list->next) {

			tracker_pt = list_entry (list, struct object_tracker, object_list);

			if (((obj_handle == object_handle) ||
				 (tracker_pt->depth == OBJECT_TRACK_DEPTH_RECURSIVE)) &&
				(tracker_pt->key_change_notify_fn != NULL))
				tracker_pt->key_change_notify_fn(type, obj_pt->parent_handle, object_handle,
												 owner_pt->object_name, owner_pt->object_name_len,
												 name_pt, name_len,
												 value_pt, value_len,
												 tracker_pt->data_pt);
		}

		hdb_handle_put (&object_instance_database, obj_handle);
		obj_handle = obj_pt->parent_handle;

	} while (obj_pt->object_handle != OBJECT_PARENT_HANDLE);
}

/*
 * object db create/destroy/set
 */
static int object_create (
	unsigned int parent_object_handle,
	unsigned int *object_handle,
	void *object_name,
	unsigned int object_name_len)
{
	struct object_instance *object_instance;
	struct object_instance *parent_instance;
	unsigned int res;
	int found = 0;
	int i;

	res = hdb_handle_get (&object_instance_database,
		parent_object_handle, (void *)&parent_instance);
	if (res != 0) {
		goto error_exit;
	}

	/*
	 * Do validation check if validation is configured for the parent object
	 */
	if (parent_instance->object_valid_list_entries) {
		for (i = 0; i < parent_instance->object_valid_list_entries; i++) {
			if ((object_name_len ==
					parent_instance->object_valid_list[i].object_len) &&
				(memcmp (object_name,
					parent_instance->object_valid_list[i].object_name,
					object_name_len) == 0)) {

				found = 1;
				break;
			}
		}

		/*
		 * Item not found in validation list
		 */
		if (found == 0) {
			goto error_object_put;
		}
	}


	res = hdb_handle_create (&object_instance_database,
		sizeof (struct object_instance), object_handle);
	if (res != 0) {
		goto error_object_put;
	}

	res = hdb_handle_get (&object_instance_database,
		*object_handle, (void *)&object_instance);
	if (res != 0) {
		goto error_destroy;
	}
	list_init (&object_instance->key_head);
	list_init (&object_instance->child_head);
	list_init (&object_instance->child_list);
	list_init (&object_instance->track_head);
	object_instance->object_name = malloc (object_name_len);
	if (object_instance->object_name == 0) {
		goto error_put_destroy;
	}
	memcpy (object_instance->object_name, object_name, object_name_len);

	object_instance->object_name_len = object_name_len;

	list_add_tail (&object_instance->child_list, &parent_instance->child_head);

	object_instance->object_handle = *object_handle;
	object_instance->find_child_list = &object_instance->child_head;
	object_instance->iter_key_list = &object_instance->key_head;
	object_instance->iter_list = &object_instance->child_head;
	object_instance->priv = NULL;
	object_instance->object_valid_list = NULL;
	object_instance->object_valid_list_entries = 0;
	object_instance->parent_handle = parent_object_handle;

	hdb_handle_put (&object_instance_database, *object_handle);

	hdb_handle_put (&object_instance_database, parent_object_handle);
	object_created_notification(object_instance->object_handle,
								object_instance->parent_handle,
								object_instance->object_name,
								object_instance->object_name_len);

	return (0);

error_put_destroy:
	hdb_handle_put (&object_instance_database, *object_handle);

error_destroy:
	hdb_handle_destroy (&object_instance_database, *object_handle);

error_object_put:
	hdb_handle_put (&object_instance_database, parent_object_handle);

error_exit:
	return (-1);
}

static int object_priv_set (
	unsigned int object_handle,
	void *priv)
{
	int res;
	struct object_instance *object_instance;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&object_instance);
	if (res != 0) {
		goto error_exit;
	}

	object_instance->priv = priv;

	hdb_handle_put (&object_instance_database, object_handle);
	return (0);

error_exit:
	return (-1);
}

static int object_key_create (
	unsigned int object_handle,
	void *key_name,
	int key_len,
	void *value,
	int value_len)
{
	struct object_instance *instance;
	struct object_key *object_key;
	unsigned int res;
	int found = 0;
	int i;
	unsigned int val;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}

	/*
	 * Do validation check if validation is configured for the parent object
	 */
	if (instance->object_key_valid_list_entries) {
		for (i = 0; i < instance->object_key_valid_list_entries; i++) {
			if ((key_len ==
					instance->object_key_valid_list[i].key_len) &&
				(memcmp (key_name,
					instance->object_key_valid_list[i].key_name,
					key_len) == 0)) {

				found = 1;
				break;
			}
		}

		/*
		 * Item not found in validation list
		 */
		if (found == 0) {
			goto error_put;
		} else {
			if (instance->object_key_valid_list[i].validate_callback) {
				res = instance->object_key_valid_list[i].validate_callback (
					key_name, key_len, value, value_len);
				if (res != 0) {
					goto error_put;
				}
			}
		}
	}

	object_key = malloc (sizeof (struct object_key));
	if (object_key == 0) {
		goto error_put;
	}
	object_key->key_name = malloc (key_len);
	if (object_key->key_name == 0) {
		goto error_put_object;
	}
	memcpy (&val, value, 4);
	object_key->value = malloc (value_len);
	if (object_key->value == 0) {
		goto error_put_key;
	}
	memcpy (object_key->key_name, key_name, key_len);
	memcpy (object_key->value, value, value_len);

	object_key->key_len = key_len;
	object_key->value_len = value_len;

	list_init (&object_key->list);
	list_add_tail (&object_key->list, &instance->key_head);
	object_key_changed_notification(object_handle, key_name, key_len,
								value, value_len, OBJECT_KEY_CREATED);

	return (0);

error_put_key:
	free (object_key->key_name);

error_put_object:
	free (object_key);

error_put:
	hdb_handle_put (&object_instance_database, object_handle);

error_exit:
	return (-1);
}

static int _clear_object(struct object_instance *instance)
{
	struct list_head *list;
	int res;
	struct object_instance *find_instance = NULL;
	struct object_key *object_key = NULL;

	for (list = instance->key_head.next;
	     list != &instance->key_head; ) {

                object_key = list_entry (list, struct object_key,
					 list);

		list = list->next;

		list_del(&object_key->list);
		free(object_key->key_name);
		free(object_key->value);
	}

	for (list = instance->child_head.next;
	     list != &instance->child_head; ) {

                find_instance = list_entry (list, struct object_instance,
					    child_list);
		res = _clear_object(find_instance);
		if (res)
			return res;

		list = list->next;

		list_del(&find_instance->child_list);
		free(find_instance->object_name);
		free(find_instance);
	}

	return 0;
}

static int object_destroy (
	unsigned int object_handle)
{
	struct object_instance *instance;
	unsigned int res;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&instance);
	if (res != 0) {
		return (res);
	}

	object_pre_deletion_notification(object_handle,
									 instance->parent_handle,
									 instance->object_name,
									 instance->object_name_len);

	/* Recursively clear sub-objects & keys */
	res = _clear_object(instance);

	list_del(&instance->child_list);
	free(instance->object_name);
	free(instance);

	return (res);
}

static int object_valid_set (
	unsigned int object_handle,
	struct object_valid *object_valid_list,
	unsigned int object_valid_list_entries)
{
	struct object_instance *instance;
	unsigned int res;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}

	instance->object_valid_list = object_valid_list;
	instance->object_valid_list_entries = object_valid_list_entries;

	hdb_handle_put (&object_instance_database, object_handle);

	return (0);

error_exit:
	return (-1);
}

static int object_key_valid_set (
		unsigned int object_handle,
		struct object_key_valid *object_key_valid_list,
		unsigned int object_key_valid_list_entries)
{
	struct object_instance *instance;
	unsigned int res;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}

	instance->object_key_valid_list = object_key_valid_list;
	instance->object_key_valid_list_entries = object_key_valid_list_entries;

	hdb_handle_put (&object_instance_database, object_handle);

	return (0);

error_exit:
	return (-1);
}

/*
 * object db reading
 */
static int object_find_create (
	unsigned int object_handle,
	void *object_name,
	int object_len,
	unsigned int *object_find_handle)
{
	unsigned int res;
	struct object_instance *object_instance;
	struct object_find_instance *object_find_instance;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&object_instance);
	if (res != 0) {
		goto error_exit;
	}

	res = hdb_handle_create (&object_find_instance_database,
		sizeof (struct object_find_instance), object_find_handle);
	if (res != 0) {
		goto error_put;
	}
	res = hdb_handle_get (&object_find_instance_database,
		*object_find_handle, (void *)&object_find_instance);
	if (res != 0) {
		goto error_destroy;
	}

	object_find_instance->find_child_list = &object_instance->child_head;
	object_find_instance->child_head = &object_instance->child_head;
	object_find_instance->object_name = object_name;
	object_find_instance->object_len = object_len;

	hdb_handle_put (&object_instance_database, object_handle);
	hdb_handle_put (&object_find_instance_database, *object_find_handle);
	return (0);

error_destroy:
	hdb_handle_destroy (&object_instance_database, *object_find_handle);

error_put:
	hdb_handle_put (&object_instance_database, object_handle);

error_exit:
	return (-1);
}

static int object_find_next (
	unsigned int object_find_handle,
	unsigned int *object_handle)
{
	unsigned int res;
	struct object_find_instance *object_find_instance;
	struct object_instance *object_instance = NULL;
	struct list_head *list;
	unsigned int found = 0;

	res = hdb_handle_get (&object_find_instance_database,
		object_find_handle, (void *)&object_find_instance);
	if (res != 0) {
		goto error_exit;
	}
	res = -1;
	for (list = object_find_instance->find_child_list->next;
		list != object_find_instance->child_head; list = list->next) {

                object_instance = list_entry (list, struct object_instance,
			child_list);

		if ((object_instance->object_name_len ==
			object_find_instance->object_len) &&

			(memcmp (object_instance->object_name,
				object_find_instance->object_name,
				object_find_instance->object_len) == 0)) {

			found = 1;
			break;
		}
	}
	object_find_instance->find_child_list = list;
	hdb_handle_put (&object_find_instance_database, object_find_handle);
	if (found) {
		*object_handle = object_instance->object_handle;
		res = 0;
	}
	return (res);

error_exit:
	return (-1);
}

static int object_find_destroy (
	unsigned int object_find_handle)
{
	return (0);
}

static int object_key_get (
	unsigned int object_handle,
	void *key_name,
	int key_len,
	void **value,
	int *value_len)
{
	unsigned int res = 0;
	struct object_instance *instance;
	struct object_key *object_key = NULL;
	struct list_head *list;
	int found = 0;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}
	for (list = instance->key_head.next;
		list != &instance->key_head; list = list->next) {

		object_key = list_entry (list, struct object_key, list);

		if ((object_key->key_len == key_len) &&
			(memcmp (object_key->key_name, key_name, key_len) == 0)) {
			found = 1;
			break;
		}
	}
	if (found) {
		*value = object_key->value;
		if (value_len) {
			*value_len = object_key->value_len;
		}
	}
	else {
		res = -1;
	}

	hdb_handle_put (&object_instance_database, object_handle);
	return (res);

error_exit:
	return (-1);
}

static int object_key_delete (
	unsigned int object_handle,
	void *key_name,
	int key_len,
	void *value,
	int value_len)
{
	unsigned int res;
	int ret = 0;
	struct object_instance *instance;
	struct object_key *object_key = NULL;
	struct list_head *list;
	int found = 0;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}
	for (list = instance->key_head.next;
		list != &instance->key_head; list = list->next) {

		object_key = list_entry (list, struct object_key, list);

		if ((object_key->key_len == key_len) &&
		    (memcmp (object_key->key_name, key_name, key_len) == 0) &&
		    (value == NULL ||
		     (object_key->value_len == value_len &&
		      (memcmp (object_key->value, value, value_len) == 0)))) {
			found = 1;
			break;
		}
	}
	if (found) {
		list_del(&object_key->list);
		free(object_key->key_name);
		free(object_key->value);
		free(object_key);
	}
	else {
		ret = -1;
		errno = ENOENT;
	}

	hdb_handle_put (&object_instance_database, object_handle);
	if (ret == 0)
		object_key_changed_notification(object_handle, key_name, key_len,
										value, value_len, OBJECT_KEY_DELETED);
	return (ret);

error_exit:
	return (-1);
}

static int object_key_replace (
	unsigned int object_handle,
	void *key_name,
	int key_len,
	void *old_value,
	int old_value_len,
	void *new_value,
	int new_value_len)
{
	unsigned int res;
	int ret = 0;
	struct object_instance *instance;
	struct object_key *object_key = NULL;
	struct list_head *list;
	int found = 0;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}
	for (list = instance->key_head.next;
		list != &instance->key_head; list = list->next) {

		object_key = list_entry (list, struct object_key, list);

		if ((object_key->key_len == key_len) &&
		    (memcmp (object_key->key_name, key_name, key_len) == 0) &&
		    (old_value == NULL ||
		     (object_key->value_len == old_value_len &&
		      (memcmp (object_key->value, old_value, old_value_len) == 0)))) {
			found = 1;
			break;
		}
	}

	if (found) {
		int i;

		/*
		 * Do validation check if validation is configured for the parent object
		 */
		if (instance->object_key_valid_list_entries) {
			for (i = 0; i < instance->object_key_valid_list_entries; i++) {
				if ((key_len ==
				     instance->object_key_valid_list[i].key_len) &&
				    (memcmp (key_name,
					     instance->object_key_valid_list[i].key_name,
					     key_len) == 0)) {

					found = 1;
					break;
				}
			}

			/*
			 * Item not found in validation list
			 */
			if (found == 0) {
				goto error_put;
			} else {
				if (instance->object_key_valid_list[i].validate_callback) {
					res = instance->object_key_valid_list[i].validate_callback (
						key_name, key_len, new_value, new_value_len);
					if (res != 0) {
						goto error_put;
					}
				}
			}
		}

		if (new_value_len <= object_key->value_len) {
			void *replacement_value;
			replacement_value = malloc(new_value_len);
			if (!replacement_value)
				goto error_exit;
			free(object_key->value);
			object_key->value = replacement_value;
		}
		memcpy(object_key->value, new_value, new_value_len);
		object_key->value_len = new_value_len;
	}
	else {
		ret = -1;
		errno = ENOENT;
	}

	hdb_handle_put (&object_instance_database, object_handle);
	if (ret == 0)
		object_key_changed_notification(object_handle, key_name, key_len,
										new_value, new_value_len, OBJECT_KEY_REPLACED);
	return (ret);

error_put:
	hdb_handle_put (&object_instance_database, object_handle);
error_exit:
	return (-1);
}

static int object_priv_get (
	unsigned int object_handle,
	void **priv)
{
	int res;
	struct object_instance *object_instance;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&object_instance);
	if (res != 0) {
		goto error_exit;
	}

	*priv = object_instance->priv;

	hdb_handle_put (&object_instance_database, object_handle);
	return (0);

error_exit:
	return (-1);
}

static int _dump_object(struct object_instance *instance, FILE *file, int depth)
{
	struct list_head *list;
	int res;
	int i;
	struct object_instance *find_instance = NULL;
	struct object_key *object_key = NULL;
	char stringbuf1[1024];
	char stringbuf2[1024];

	memcpy(stringbuf1, instance->object_name, instance->object_name_len);
	stringbuf1[instance->object_name_len] = '\0';

	for (i=0; i<depth; i++)
		fprintf(file, "    ");

	if (instance->object_handle != OBJECT_PARENT_HANDLE)
		fprintf(file, "%s {\n", stringbuf1);

	for (list = instance->key_head.next;
	     list != &instance->key_head; list = list->next) {

                object_key = list_entry (list, struct object_key,
					 list);

		memcpy(stringbuf1, object_key->key_name, object_key->key_len);
		stringbuf1[object_key->key_len] = '\0';
		memcpy(stringbuf2, object_key->value, object_key->value_len);
		stringbuf2[object_key->value_len] = '\0';

		for (i=0; i<depth+1; i++)
			fprintf(file, "    ");

		fprintf(file, "%s: %s\n", stringbuf1, stringbuf2);
	}

	for (list = instance->child_head.next;
	     list != &instance->child_head; list = list->next) {

                find_instance = list_entry (list, struct object_instance,
					    child_list);
		res = _dump_object(find_instance, file, depth+1);
		if (res)
			return res;
	}
	for (i=0; i<depth; i++)
		fprintf(file, "    ");

	if (instance->object_handle != OBJECT_PARENT_HANDLE)
		fprintf(file, "}\n");

	return 0;
}


static int object_key_iter_reset(unsigned int object_handle)
{
	unsigned int res;
	struct object_instance *instance;

	res = hdb_handle_get (&object_instance_database,
		object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}
	instance->iter_key_list = &instance->key_head;

	hdb_handle_put (&object_instance_database, object_handle);
	return (0);

error_exit:
	return (-1);
}

static int object_key_iter(unsigned int parent_object_handle,
			   void **key_name,
			   int *key_len,
			   void **value,
			   int *value_len)
{
	unsigned int res;
	struct object_instance *instance;
	struct object_key *find_key = NULL;
	struct list_head *list;
	unsigned int found = 0;

	res = hdb_handle_get (&object_instance_database,
		parent_object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}
	res = -ENOENT;
	list = instance->iter_key_list->next;
	if (list != &instance->key_head) {
                find_key = list_entry (list, struct object_key, list);
		found = 1;
	}
	instance->iter_key_list = list;
	if (found) {
		*key_name = find_key->key_name;
		if (key_len)
			*key_len = find_key->key_len;
		*value = find_key->value;
		if (value_len)
			*value_len = find_key->value_len;
		res = 0;
	}
	else {
		res = -1;
	}

	hdb_handle_put (&object_instance_database, parent_object_handle);
	return (res);

error_exit:
	return (-1);
}

static int object_iter_reset(unsigned int parent_object_handle)
{
	unsigned int res;
	struct object_instance *instance;

	res = hdb_handle_get (&object_instance_database,
		parent_object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}
	instance->iter_list = &instance->child_head;

	hdb_handle_put (&object_instance_database, parent_object_handle);
	return (0);

error_exit:
	return (-1);
}

static int object_iter(unsigned int parent_object_handle,
		       void **object_name,
		       int *name_len,
		       unsigned int *object_handle)
{
	unsigned int res;
	struct object_instance *instance;
	struct object_instance *find_instance = NULL;
	struct list_head *list;
	unsigned int found = 0;

	res = hdb_handle_get (&object_instance_database,
		parent_object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}
	res = -ENOENT;
	list = instance->iter_list->next;
	if (list != &instance->child_head) {

                find_instance = list_entry (list, struct object_instance,
					    child_list);
		found = 1;
	}
	instance->iter_list = list;

	if (found) {
		*object_handle = find_instance->object_handle;
		*object_name = find_instance->object_name;
		*name_len = find_instance->object_name_len;
		res = 0;
	}
	else {
		res = -1;
	}

	return (res);

error_exit:
	return (-1);
}


static int object_find_from(unsigned int parent_object_handle,
			    unsigned int start_pos,
			    void *object_name,
			    int object_name_len,
			    unsigned int *object_handle,
			    unsigned int *next_pos)
{
	unsigned int res;
	unsigned int pos = 0;
	struct object_instance *instance;
	struct object_instance *find_instance = NULL;
	struct list_head *list;
	unsigned int found = 0;

	res = hdb_handle_get (&object_instance_database,
		parent_object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}
	res = -ENOENT;
	for (list = instance->child_head.next;
		list != &instance->child_head; list = list->next) {

                find_instance = list_entry (list, struct object_instance,
			child_list);

		if ((find_instance->object_name_len == object_name_len) &&
			(memcmp (find_instance->object_name, object_name,
			object_name_len) == 0)) {
			if (pos++ == start_pos) {
				found = 1;
				break;
			}
		}
	}

	hdb_handle_put (&object_instance_database, parent_object_handle);
	if (found) {
		*object_handle = find_instance->object_handle;
		res = 0;
	}
	*next_pos = pos;
	return (res);

error_exit:
	return (-1);
}

static int object_iter_from(unsigned int parent_object_handle,
			    unsigned int start_pos,
			    void **object_name,
			    int *name_len,
			    unsigned int *object_handle)
{
	unsigned int res;
	unsigned int pos = 0;
	struct object_instance *instance;
	struct object_instance *find_instance = NULL;
	struct list_head *list;
	unsigned int found = 0;

	res = hdb_handle_get (&object_instance_database,
		parent_object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}
	res = -ENOENT;

	for (list = instance->child_head.next;
		list != &instance->child_head; list = list->next) {

                find_instance = list_entry (list, struct object_instance,
					    child_list);
		if (pos++ == start_pos) {
			found = 1;
			break;
		}
	}

	if (found) {
		*object_handle = find_instance->object_handle;
		*object_name = find_instance->object_name;
		*name_len = find_instance->object_name_len;
		res = 0;
	}
	else {
		res = -1;
	}

	return (res);

error_exit:
	return (-1);
}

static int object_key_iter_from(unsigned int parent_object_handle,
				unsigned int start_pos,
				void **key_name,
				int *key_len,
				void **value,
				int *value_len)
{
	unsigned int pos = 0;
	unsigned int res;
	struct object_instance *instance;
	struct object_key *find_key = NULL;
	struct list_head *list;
	unsigned int found = 0;

	res = hdb_handle_get (&object_instance_database,
		parent_object_handle, (void *)&instance);
	if (res != 0) {
		goto error_exit;
	}
	res = -ENOENT;

	for (list = instance->key_head.next;
		list != &instance->key_head; list = list->next) {

		find_key = list_entry (list, struct object_key, list);

		if (pos++ == start_pos) {
			found = 1;
			break;
		}
	}

	if (found) {
		*key_name = find_key->key_name;
		if (key_len)
			*key_len = find_key->key_len;
		*value = find_key->value;
		if (value_len)
			*value_len = find_key->value_len;
		res = 0;
	}
	else {
		res = -1;
	}

	hdb_handle_put (&object_instance_database, parent_object_handle);
	return (res);

error_exit:
	return (-1);
}


static int object_parent_get(unsigned int object_handle,
			     unsigned int *parent_handle)
{
	struct object_instance *instance;
	unsigned int res;

	res = hdb_handle_get (&object_instance_database,
			      object_handle, (void *)&instance);
	if (res != 0) {
		return (res);
	}

	if (object_handle == OBJECT_PARENT_HANDLE)
		*parent_handle = 0;
	else
		*parent_handle = instance->parent_handle;

	hdb_handle_put (&object_instance_database, object_handle);

	return (0);
}


static int object_track_start(unsigned int object_handle,
							  object_track_depth_t depth,
							  object_key_change_notify_fn_t key_change_notify_fn,
							  object_create_notify_fn_t object_create_notify_fn,
							  object_destroy_notify_fn_t object_destroy_notify_fn,
							  void * priv_data_pt)
{
	struct object_instance *instance;
	unsigned int res;
	struct object_tracker * tracker_pt;

	res = hdb_handle_get (&object_instance_database,
			      object_handle, (void *)&instance);
	if (res != 0) {
		return (res);
	}
	tracker_pt = malloc(sizeof(struct object_tracker));

	tracker_pt->depth = depth;
	tracker_pt->object_handle = object_handle;
	tracker_pt->key_change_notify_fn = key_change_notify_fn;
	tracker_pt->object_create_notify_fn = object_create_notify_fn;
	tracker_pt->object_destroy_notify_fn = object_destroy_notify_fn;
	tracker_pt->data_pt = priv_data_pt;

	list_init(&tracker_pt->object_list);
	list_init(&tracker_pt->tracker_list);

	list_add(&tracker_pt->object_list, &instance->track_head);
	list_add(&tracker_pt->tracker_list, &objdb_trackers_head);

	hdb_handle_put (&object_instance_database, object_handle);

	return (res);
}

static void object_track_stop(object_key_change_notify_fn_t key_change_notify_fn,
							  object_create_notify_fn_t object_create_notify_fn,
							  object_destroy_notify_fn_t object_destroy_notify_fn,
							  void * priv_data_pt)
{
	struct object_instance *instance;
	struct object_tracker * tracker_pt = NULL;
	struct object_tracker * obj_tracker_pt = NULL;
	struct list_head *list, *tmp_list;
	struct list_head *obj_list, *tmp_obj_list;
	unsigned int res;

	/* go through the global list and find all the trackers to stop */
	for (list = objdb_trackers_head.next, tmp_list = list->next;
		 list != &objdb_trackers_head; list = tmp_list, tmp_list = tmp_list->next) {

		tracker_pt = list_entry (list, struct object_tracker, tracker_list);

		if (tracker_pt && (tracker_pt->data_pt == priv_data_pt) &&
			(tracker_pt->object_create_notify_fn == object_create_notify_fn) &&
			(tracker_pt->object_destroy_notify_fn == object_destroy_notify_fn) &&
			(tracker_pt->key_change_notify_fn == key_change_notify_fn)) {

			/* get the object & take this tracker off of it's list. */

			res = hdb_handle_get (&object_instance_database,
								  tracker_pt->object_handle, (void *)&instance);
			if (res != 0) continue;

			for (obj_list = instance->track_head.next, tmp_obj_list = obj_list->next;
				 obj_list != &instance->track_head; obj_list = tmp_obj_list, tmp_obj_list = tmp_obj_list->next) {

				obj_tracker_pt = list_entry (obj_list, struct object_tracker, object_list);
				if (obj_tracker_pt == tracker_pt) {
					/* this is the tracker we are after. */
					list_del(obj_list);
				}
			}
			hdb_handle_put (&object_instance_database, tracker_pt->object_handle);

			/* remove the tracker off of the global list */
			list_del(list);
			free(tracker_pt);
		}
	}
}

static int object_dump(unsigned int object_handle,
		       FILE *file)
{
	struct object_instance *instance;
	unsigned int res;

	res = hdb_handle_get (&object_instance_database,
			      object_handle, (void *)&instance);
	if (res != 0) {
		return (res);
	}

	res = _dump_object(instance, file, -1);

	hdb_handle_put (&object_instance_database, object_handle);

	return (res);
}

static int object_write_config(char **error_string)
{
	struct config_iface_ver0 **modules;
	int num_modules;
	int i;
	int res;

	main_get_config_modules(&modules, &num_modules);
	for (i=0; i<num_modules; i++) {
		if (modules[i]->config_writeconfig) {
			res = modules[i]->config_writeconfig(&objdb_iface, error_string);
			if (res)
				return res;
		}
	}
	return 0;
}

static int object_reload_config(int flush, char **error_string)
{
	struct config_iface_ver0 **modules;
	int num_modules;
	int i;
	int res;

	main_get_config_modules(&modules, &num_modules);

	for (i=0; i<num_modules; i++) {
		if (modules[i]->config_reloadconfig) {
			res = modules[i]->config_reloadconfig(&objdb_iface, flush, error_string);
			if (res)
				return res;
		}
	}
	return 0;
}

struct objdb_iface_ver0 objdb_iface = {
	.objdb_init		= objdb_init,
	.object_create		= object_create,
	.object_priv_set	= object_priv_set,
	.object_key_create	= object_key_create,
	.object_key_delete	= object_key_delete,
	.object_key_replace	= object_key_replace,
	.object_destroy		= object_destroy,
	.object_valid_set	= object_valid_set,
	.object_key_valid_set	= object_key_valid_set,
	.object_find_create	= object_find_create,
	.object_find_next	= object_find_next,
	.object_find_destroy	= object_find_destroy,
	.object_find_from	= object_find_from,
	.object_key_get		= object_key_get,
	.object_key_iter	= object_key_iter,
	.object_key_iter_reset	= object_key_iter_reset,
	.object_key_iter_from	= object_key_iter_from,
	.object_iter	        = object_iter,
	.object_iter_reset	= object_iter_reset,
	.object_iter_from	= object_iter_from,
	.object_priv_get	= object_priv_get,
	.object_parent_get	= object_parent_get,
	.object_track_start	= object_track_start,
	.object_track_stop	= object_track_stop,
	.object_dump	        = object_dump,
	.object_write_config    = object_write_config,
	.object_reload_config   = object_reload_config,
};

struct lcr_iface objdb_iface_ver0[1] = {
	{
		.name			= "objdb",
		.version		= 0,
		.versions_replace	= 0,
		.versions_replace_count = 0,
		.dependencies		= 0,
		.dependency_count	= 0,
		.constructor		= NULL,
		.destructor		= NULL,
		.interfaces		= NULL,
	}
};

struct lcr_comp objdb_comp_ver0 = {
	.iface_count			= 1,
	.ifaces				= objdb_iface_ver0
};



__attribute__ ((constructor)) static void objdb_comp_register (void) {
        lcr_interfaces_set (&objdb_iface_ver0[0], &objdb_iface);

	lcr_component_register (&objdb_comp_ver0);
}
