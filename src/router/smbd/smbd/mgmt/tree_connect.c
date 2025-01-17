// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 */

#include <linux/list.h>
#include <linux/slab.h>

#include "../buffer_pool.h"
#include "../transport_ipc.h"
#include "../connection.h"

#include "tree_connect.h"
#include "user_config.h"
#include "share_config.h"
#include "user_session.h"

struct ksmbd_tree_conn_status
ksmbd_tree_conn_connect(struct ksmbd_conn *conn, struct ksmbd_session *sess,
			const char *share_name)
{
	struct ksmbd_tree_conn_status status = {-ENOENT, NULL};
	struct ksmbd_tree_connect_response *resp = NULL;
	struct ksmbd_share_config *sc;
	struct ksmbd_tree_connect *tree_conn = NULL;
	struct sockaddr *peer_addr;
	int ret;

	sc = ksmbd_share_config_get(conn->um, share_name);
	if (!sc)
		return status;

	tree_conn = ksmbd_zalloc(sizeof(struct ksmbd_tree_connect));
	if (!tree_conn) {
		status.ret = -ENOMEM;
		goto out_error;
	}

	tree_conn->id = ksmbd_acquire_tree_conn_id(sess);
	if (tree_conn->id < 0) {
		status.ret = -EINVAL;
		goto out_error;
	}

	peer_addr = KSMBD_TCP_PEER_SOCKADDR(conn);
	resp = ksmbd_ipc_tree_connect_request(sess,
					      sc,
					      tree_conn,
					      peer_addr);
	if (!resp) {
		status.ret = -EINVAL;
		goto out_error;
	}

	status.ret = resp->status;
	if (status.ret != KSMBD_TREE_CONN_STATUS_OK)
		goto out_error;

	tree_conn->flags = resp->connection_flags;
	if (test_tree_conn_flag(tree_conn, KSMBD_TREE_CONN_FLAG_UPDATE)) {
		struct ksmbd_share_config *new_sc;

		ksmbd_share_config_del(sc);
		new_sc = ksmbd_share_config_get(conn->um, share_name);
		if (!new_sc) {
			pr_err("Failed to update stale share config\n");
			status.ret = -ESTALE;
			goto out_error;
		}
		ksmbd_share_config_put(sc);
		sc = new_sc;
	}

	tree_conn->user = sess->user;
	tree_conn->share_conf = sc;
	tree_conn->t_state = TREE_NEW;
	status.tree_conn = tree_conn;
	atomic_set(&tree_conn->refcount, 1);
	init_waitqueue_head(&tree_conn->refcount_q);

	list_add(&tree_conn->list, &sess->tree_conn_list);
	ksmbd_free(resp);
	return status;

out_error:
	if (tree_conn)
		ksmbd_release_tree_conn_id(sess, tree_conn->id);
	ksmbd_share_config_put(sc);
	ksmbd_free(tree_conn);
	ksmbd_free(resp);
	return status;
}

void ksmbd_tree_connect_put(struct ksmbd_tree_connect *tcon)
{
    /*
   * Checking waitqueue to releasing tree connect on
   * tree disconnect. waitqueue_active is safe because it
   * uses atomic operation for condition.
   */
	if (!atomic_dec_return(&tcon->refcount) &&
	    waitqueue_active(&tcon->refcount_q))
		wake_up(&tcon->refcount_q);
}

int ksmbd_tree_conn_disconnect(struct ksmbd_session *sess,
			       struct ksmbd_tree_connect *tree_conn)
{
	int ret;

	if (!atomic_dec_and_test(&tree_conn->refcount))
		wait_event(tree_conn->refcount_q,
			   atomic_read(&tree_conn->refcount) == 0);
	ret = ksmbd_ipc_tree_disconnect_request(sess->id, tree_conn->id);
	ksmbd_release_tree_conn_id(sess, tree_conn->id);
	write_lock(&sess->tree_conns_lock);
	list_del(&tree_conn->list);
	write_unlock(&sess->tree_conns_lock);
	ksmbd_share_config_put(tree_conn->share_conf);
	ksmbd_free(tree_conn);
	return ret;
}

struct ksmbd_tree_connect *ksmbd_tree_conn_lookup(struct ksmbd_session *sess,
						  unsigned int id)
{
	struct ksmbd_tree_connect *tree_conn;
	struct ksmbd_tree_connect *tcon;
	struct list_head *tmp;

	read_lock(&sess->tree_conns_lock);
	list_for_each(tmp, &sess->tree_conn_list) {
		tree_conn = list_entry(tmp, struct ksmbd_tree_connect, list);
		if (tree_conn->id == id) {
			tcon = tree_conn;
			break;
		}
	}
	if (tcon) {
		if (tcon->t_state != TREE_CONNECTED)
			tcon = NULL;
		else if (!atomic_inc_not_zero(&tcon->refcount))
			tcon = NULL;
 	}
	read_unlock(&sess->tree_conns_lock);
	return tcon;
}

int ksmbd_tree_conn_session_logoff(struct ksmbd_session *sess)
{
	int ret = 0;
	while (!list_empty(&sess->tree_conn_list)) {
		struct ksmbd_tree_connect *tc;
		tc = list_entry(sess->tree_conn_list.next,
				struct ksmbd_tree_connect,
				list);
		write_lock(&sess->tree_conns_lock);
		if (tc->t_state == TREE_DISCONNECTED) {
			write_unlock(&sess->tree_conns_lock);
			ret = -ENOENT;
			continue;
		}
		tc->t_state = TREE_DISCONNECTED;
		write_unlock(&sess->tree_conns_lock);
		ret |= ksmbd_tree_conn_disconnect(sess, tc);
	}
	return ret;
}
