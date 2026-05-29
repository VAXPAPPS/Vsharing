/*
 * vfs_bridge.c — جسر نظام الملفات الافتراضي (FUSE3)
 */

#define FUSE_USE_VERSION 31

#include "vfs_bridge.h"
#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ============================================================
 * الهياكل الداخلية وحالة النظام
 * ============================================================ */

typedef struct {
    uint64_t req_id;
    gboolean completed;
    int32_t  error;
    uint8_t *resp_data;
    uint32_t resp_len;
    GMutex   mutex;
    GCond    cond;
} PendingRequest;

typedef struct {
    VfsSendFrameCb send_cb;
    gpointer       user_data;
    GHashTable    *pending_reqs;
    GMutex         req_mutex;
    uint64_t       next_req_id;

    GHashTable    *mounted_devices; /* device_id -> VfsMountedDevice* */
    GMutex         dev_mutex;
} VfsContext;

static VfsContext g_vfs = {0};

/* ============================================================
 * إدارة الطلبات (RPC)
 * ============================================================ */

static PendingRequest *req_new(uint64_t req_id) {
    PendingRequest *req = g_new0(PendingRequest, 1);
    req->req_id = req_id;
    g_mutex_init(&req->mutex);
    g_cond_init(&req->cond);
    return req;
}

static void req_free(PendingRequest *req) {
    if (!req) return;
    g_mutex_clear(&req->mutex);
    g_cond_clear(&req->cond);
    if (req->resp_data) g_free(req->resp_data);
    g_free(req);
}

static int32_t vfs_rpc_call(const char *device_id, VfsOpCode op, const uint8_t *payload, uint32_t payload_len, const char *path, uint8_t **out_data, uint32_t *out_len) {
    if (!g_vfs.send_cb) return -ENOSYS;

    g_mutex_lock(&g_vfs.req_mutex);
    uint64_t req_id = ++g_vfs.next_req_id;
    PendingRequest *req = req_new(req_id);
    g_hash_table_insert(g_vfs.pending_reqs, GUINT_TO_POINTER((guint)req_id), req);
    g_mutex_unlock(&g_vfs.req_mutex);

    GBytes *frame = vlink_make_vfs_request(req_id, op, payload, payload_len, path);
    if (!frame) {
        g_mutex_lock(&g_vfs.req_mutex);
        g_hash_table_remove(g_vfs.pending_reqs, GUINT_TO_POINTER((guint)req_id));
        g_mutex_unlock(&g_vfs.req_mutex);
        return -ENOMEM;
    }

    if (!g_vfs.send_cb(frame, g_vfs.user_data)) {
        g_bytes_unref(frame);
        g_mutex_lock(&g_vfs.req_mutex);
        g_hash_table_remove(g_vfs.pending_reqs, GUINT_TO_POINTER((guint)req_id));
        g_mutex_unlock(&g_vfs.req_mutex);
        return -EIO;
    }
    g_bytes_unref(frame);

    /* انتظار الرد */
    g_mutex_lock(&req->mutex);
    int64_t end_time = g_get_monotonic_time() + VFS_REQUEST_TIMEOUT_MS * 1000;
    while (!req->completed) {
        if (!g_cond_wait_until(&req->cond, &req->mutex, end_time)) {
            req->error = -ETIMEDOUT;
            break;
        }
    }
    
    int32_t err = req->error;
    if (out_data && out_len && err == 0 && req->resp_data) {
        *out_data = g_memdup2(req->resp_data, req->resp_len);
        *out_len = req->resp_len;
    } else if (out_data) {
        *out_data = NULL;
        *out_len = 0;
    }
    g_mutex_unlock(&req->mutex);

    g_mutex_lock(&g_vfs.req_mutex);
    g_hash_table_remove(g_vfs.pending_reqs, GUINT_TO_POINTER((guint)req_id));
    g_mutex_unlock(&g_vfs.req_mutex);

    return err;
}

void vfs_bridge_on_response(uint8_t msg_type, const uint8_t *payload, uint32_t len) {
    if (msg_type != VLINK_FILE_LIST_RESP && msg_type != VLINK_FILE_READ_RESP) return;
    if (len < sizeof(VfsRpcRespHeader)) return;

    const VfsRpcRespHeader *hdr = (const VfsRpcRespHeader *)payload;
    uint64_t req_id = GUINT64_FROM_BE(hdr->req_id);

    g_mutex_lock(&g_vfs.req_mutex);
    PendingRequest *req = g_hash_table_lookup(g_vfs.pending_reqs, GUINT_TO_POINTER((guint)req_id));
    if (req) {
        g_mutex_lock(&req->mutex);
        req->error = (int32_t)GUINT32_FROM_BE((uint32_t)hdr->error);
        uint32_t data_len = len - sizeof(VfsRpcRespHeader);
        if (data_len > 0) {
            req->resp_data = g_memdup2(payload + sizeof(VfsRpcRespHeader), data_len);
            req->resp_len = data_len;
        }
        req->completed = TRUE;
        g_cond_signal(&req->cond);
        g_mutex_unlock(&req->mutex);
    }
    g_mutex_unlock(&g_vfs.req_mutex);
}

/* ============================================================
 * عمليات FUSE الأساسية
 * ============================================================ */

static int vfs_fuse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));
    
    uint8_t *resp = NULL;
    uint32_t resp_len = 0;
    int32_t err = vfs_rpc_call("default", VFS_OP_STAT, NULL, 0, path, &resp, &resp_len);
    
    if (err == 0 && resp && resp_len >= sizeof(VfsStatInfo)) {
        VfsStatInfo *info = (VfsStatInfo *)resp;
        stbuf->st_size = info->file_size;
        stbuf->st_mode = info->mode;
        stbuf->st_uid  = info->uid;
        stbuf->st_gid  = info->gid;
        stbuf->st_nlink = info->nlink;
        stbuf->st_atime = info->atime_sec;
        stbuf->st_mtime = info->mtime_sec;
        stbuf->st_ctime = info->ctime_sec;
        stbuf->st_blksize = info->blksize;
        stbuf->st_blocks = info->blocks;
    }
    if (resp) g_free(resp);
    return err;
}

static int vfs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    
    uint8_t *resp = NULL;
    uint32_t resp_len = 0;
    int32_t err = vfs_rpc_call("default", VFS_OP_READDIR, NULL, 0, path, &resp, &resp_len);
    
    if (err == 0 && resp) {
        uint32_t off = 0;
        while (off + sizeof(VfsDirEntry) <= resp_len) {
            VfsDirEntry *entry = (VfsDirEntry *)(resp + off);
            uint32_t entry_size = sizeof(VfsDirEntry) + entry->name_len;
            if (off + entry_size > resp_len) break;
            
            char *name = g_strndup((const char *)(resp + off + sizeof(VfsDirEntry)), entry->name_len);
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_mode = entry->mode;
            st.st_size = entry->file_size;
            st.st_mtime = entry->mtime_sec;
            
            filler(buf, name, &st, 0, 0);
            g_free(name);
            off += entry_size;
        }
    }
    if (resp) g_free(resp);
    return err;
}

static int vfs_fuse_open(const char *path, struct fuse_file_info *fi) {
    int32_t err = vfs_rpc_call("default", VFS_OP_OPEN, NULL, 0, path, NULL, NULL);
    return err;
}

static int vfs_fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    VfsReadParams params = {
        .offset = offset,
        .length = size,
        .path_len = strlen(path)
    };
    
    uint8_t *payload = g_malloc(sizeof(VfsReadParams) + params.path_len);
    memcpy(payload, &params, sizeof(VfsReadParams));
    memcpy(payload + sizeof(VfsReadParams), path, params.path_len);
    
    uint8_t *resp = NULL;
    uint32_t resp_len = 0;
    int32_t err = vfs_rpc_call("default", VFS_OP_READ, payload, sizeof(VfsReadParams) + params.path_len, path, &resp, &resp_len);
    g_free(payload);
    
    if (err == 0 && resp && resp_len >= sizeof(VfsReadResponse)) {
        VfsReadResponse *rr = (VfsReadResponse *)resp;
        if (resp_len >= sizeof(VfsReadResponse) + rr->bytes_read) {
            memcpy(buf, resp + sizeof(VfsReadResponse), rr->bytes_read);
            err = rr->bytes_read;
        } else {
            err = -EIO;
        }
    }
    if (resp) g_free(resp);
    return err < 0 ? err : err;
}

static struct fuse_operations vfs_oper = {
    .getattr = vfs_fuse_getattr,
    .readdir = vfs_fuse_readdir,
    .open    = vfs_fuse_open,
    .read    = vfs_fuse_read,
};

/* ============================================================
 * دوال التهيئة والإدارة
 * ============================================================ */

gboolean vfs_bridge_init(VfsSendFrameCb send_cb, gpointer user_data) {
    g_vfs.send_cb = send_cb;
    g_vfs.user_data = user_data;
    g_vfs.pending_reqs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)req_free);
    g_mutex_init(&g_vfs.req_mutex);
    
    g_vfs.mounted_devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_mutex_init(&g_vfs.dev_mutex);
    
    return TRUE;
}

static gpointer fuse_thread_func(gpointer data) {
    VfsMountedDevice *dev = (VfsMountedDevice *)data;
    
    char *argv[] = {
        "vsharing_vfs",
        "-f",
        "-o", VFS_MOUNT_OPTIONS,
        dev->mount_path,
        NULL
    };
    
    g_print("[VFS] 📂 Mounting FUSE at %s\n", dev->mount_path);
    fuse_main(5, argv, &vfs_oper, dev);
    g_print("[VFS] 🔌 Unmounted %s\n", dev->mount_path);
    
    return NULL;
}

char *vfs_bridge_mount_device(const char *device_id, const char *device_name) {
    if (!device_id || !device_name) return NULL;
    
    char *home = (char *)g_get_home_dir();
    char *mount_dir = g_build_filename(home, VFS_MOUNT_BASE_DIR, device_name, NULL);
    
    /* إنشاء المجلد إذا لم يكن موجوداً */
    g_mkdir_with_parents(mount_dir, 0755);
    
    VfsMountedDevice *dev = g_new0(VfsMountedDevice, 1);
    g_strlcpy(dev->device_id, device_id, sizeof(dev->device_id));
    g_strlcpy(dev->device_name, device_name, sizeof(dev->device_name));
    g_strlcpy(dev->mount_path, mount_dir, sizeof(dev->mount_path));
    dev->mounted_at = g_get_monotonic_time() / 1000;
    dev->active = TRUE;
    
    g_mutex_lock(&g_vfs.dev_mutex);
    g_hash_table_insert(g_vfs.mounted_devices, g_strdup(device_id), dev);
    g_mutex_unlock(&g_vfs.dev_mutex);
    
    dev->fuse_thread = g_thread_new("fuse-thread", fuse_thread_func, dev);
    
    return mount_dir;
}

void vfs_bridge_unmount_device(const char *device_id) {
    g_mutex_lock(&g_vfs.dev_mutex);
    VfsMountedDevice *dev = g_hash_table_lookup(g_vfs.mounted_devices, device_id);
    if (dev && dev->active) {
        dev->active = FALSE;
        /* نطلب من FUSE إلغاء التركيب بشكل نظيف باستخدام fusermount3 */
        char *cmd = g_strdup_printf("fusermount3 -u \"%s\"", dev->mount_path);
        system(cmd);
        g_free(cmd);
        
        if (dev->fuse_thread) {
            g_thread_join(dev->fuse_thread);
            dev->fuse_thread = NULL;
        }
        g_hash_table_remove(g_vfs.mounted_devices, device_id);
    }
    g_mutex_unlock(&g_vfs.dev_mutex);
}

void vfs_bridge_unmount_all(void) {
    g_mutex_lock(&g_vfs.dev_mutex);
    GList *keys = g_hash_table_get_keys(g_vfs.mounted_devices);
    for (GList *l = keys; l != NULL; l = l->next) {
        char *device_id = (char *)l->data;
        VfsMountedDevice *dev = g_hash_table_lookup(g_vfs.mounted_devices, device_id);
        if (dev && dev->active) {
            char *cmd = g_strdup_printf("fusermount3 -u \"%s\"", dev->mount_path);
            system(cmd);
            g_free(cmd);
            if (dev->fuse_thread) {
                g_thread_join(dev->fuse_thread);
                dev->fuse_thread = NULL;
            }
        }
    }
    g_list_free(keys);
    g_hash_table_remove_all(g_vfs.mounted_devices);
    g_mutex_unlock(&g_vfs.dev_mutex);
}

void vfs_bridge_cleanup(void) {
    vfs_bridge_unmount_all();
    g_hash_table_unref(g_vfs.mounted_devices);
    g_hash_table_unref(g_vfs.pending_reqs);
    g_mutex_clear(&g_vfs.req_mutex);
    g_mutex_clear(&g_vfs.dev_mutex);
}
