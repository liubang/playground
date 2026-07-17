-- MiniDFS MySQL Schema
-- This file defines all tables needed by MySQLMetadataStore.

CREATE DATABASE IF NOT EXISTS minidfs DEFAULT CHARACTER SET utf8mb4;
USE minidfs;

-- ============================================================================
-- Inodes: namespace tree
-- ============================================================================
CREATE TABLE IF NOT EXISTS inodes (
    inode_id        BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    type            TINYINT UNSIGNED NOT NULL COMMENT '1=directory, 2=file',
    parent_id       BIGINT UNSIGNED NOT NULL DEFAULT 0,
    name            VARCHAR(255) NOT NULL,
    owner           VARCHAR(128) NOT NULL DEFAULT '',
    `group`         VARCHAR(128) NOT NULL DEFAULT '',
    permission      INT UNSIGNED NOT NULL DEFAULT 755,
    length          BIGINT UNSIGNED NOT NULL DEFAULT 0,
    replication     INT UNSIGNED NOT NULL DEFAULT 3,
    block_size      BIGINT UNSIGNED NOT NULL DEFAULT 134217728,
    file_append_mode TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=appendable,1=immutable_after_complete',
    content_generation BIGINT UNSIGNED NOT NULL DEFAULT 0,
    checksum        INT UNSIGNED NOT NULL DEFAULT 0,
    checksum_valid  BOOLEAN NOT NULL DEFAULT FALSE,
    state           TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=normal,1=under_construction,2=deleted',
    ctime_ms        BIGINT UNSIGNED NOT NULL DEFAULT 0,
    mtime_ms        BIGINT UNSIGNED NOT NULL DEFAULT 0,
    version         BIGINT UNSIGNED NOT NULL DEFAULT 0,
    UNIQUE KEY uk_parent_name (parent_id, name),
    KEY idx_parent (parent_id)
) ENGINE=InnoDB;

-- Root inode (inode_id=1, parent_id=0)
INSERT IGNORE INTO inodes (inode_id, type, parent_id, name, owner, `group`, permission, ctime_ms, mtime_ms)
VALUES (1, 1, 0, '/', 'root', 'supergroup', 755, UNIX_TIMESTAMP()*1000, UNIX_TIMESTAMP()*1000);

-- ============================================================================
-- Blocks: block metadata (one row per block)
-- ============================================================================
CREATE TABLE IF NOT EXISTS blocks (
    block_id        BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    inode_id        BIGINT UNSIGNED NOT NULL,
    block_index     INT UNSIGNED NOT NULL,
    generation_stamp BIGINT UNSIGNED NOT NULL DEFAULT 0,
    length          BIGINT UNSIGNED NOT NULL DEFAULT 0,
    state           TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=allocating,1=committed,2=corrupt,3=deleted',
    desired_replica INT UNSIGNED NOT NULL DEFAULT 3,
    ctime_ms        BIGINT UNSIGNED NOT NULL DEFAULT 0,
    mtime_ms        BIGINT UNSIGNED NOT NULL DEFAULT 0,
    UNIQUE KEY uk_inode_block_index (inode_id, block_index),
    KEY idx_inode (inode_id, block_index),
    KEY idx_state (state)
) ENGINE=InnoDB;

-- ============================================================================
-- Block Replicas: which datanode holds which block
-- ============================================================================
CREATE TABLE IF NOT EXISTS block_replicas (
    block_id        BIGINT UNSIGNED NOT NULL,
    datanode_id     BIGINT UNSIGNED NOT NULL,
    storage_id      BIGINT UNSIGNED NOT NULL DEFAULT 0,
    state           TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=writing,1=finalized,2=corrupt,3=stale,4=deleting,5=deleted',
    length          BIGINT UNSIGNED NOT NULL DEFAULT 0,
    generation_stamp BIGINT UNSIGNED NOT NULL DEFAULT 0,
    report_time_ms  BIGINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (block_id, datanode_id, storage_id),
    KEY idx_datanode (datanode_id)
) ENGINE=InnoDB;

-- ============================================================================
-- DataNodes: registered datanodes
-- ============================================================================
CREATE TABLE IF NOT EXISTS datanodes (
    datanode_id     BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    uuid            VARCHAR(64) NOT NULL,
    hostname        VARCHAR(255) NOT NULL DEFAULT '',
    ip              VARCHAR(45) NOT NULL DEFAULT '',
    rpc_port        INT UNSIGNED NOT NULL DEFAULT 0,
    data_port       INT UNSIGNED NOT NULL DEFAULT 0,
    rack            VARCHAR(255) NOT NULL DEFAULT '/default-rack',
    state           TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=live,1=stale,2=dead,3=decommissioning,4=decommissioned',
    capacity_bytes  BIGINT UNSIGNED NOT NULL DEFAULT 0,
    used_bytes      BIGINT UNSIGNED NOT NULL DEFAULT 0,
    free_bytes      BIGINT UNSIGNED NOT NULL DEFAULT 0,
    last_heartbeat_ms BIGINT UNSIGNED NOT NULL DEFAULT 0,
    UNIQUE KEY uk_uuid (uuid)
) ENGINE=InnoDB;

-- ============================================================================
-- Leases: file write leases
-- ============================================================================
CREATE TABLE IF NOT EXISTS leases (
    lease_id        BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    inode_id        BIGINT UNSIGNED NOT NULL,
    client_id       VARCHAR(128) NOT NULL,
    state           TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=active,1=closed',
    expire_time_ms  BIGINT UNSIGNED NOT NULL DEFAULT 0,
    ctime_ms        BIGINT UNSIGNED NOT NULL DEFAULT 0,
    mtime_ms        BIGINT UNSIGNED NOT NULL DEFAULT 0,
    KEY idx_inode_state (inode_id, state),
    KEY idx_expire (state, expire_time_ms)
) ENGINE=InnoDB;

-- ============================================================================
-- ID Allocators: monotonic ID generation
-- ============================================================================
CREATE TABLE IF NOT EXISTS id_allocators (
    name            VARCHAR(64) NOT NULL PRIMARY KEY,
    next_id         BIGINT UNSIGNED NOT NULL DEFAULT 0
) ENGINE=InnoDB;

-- Initialize allocators
INSERT IGNORE INTO id_allocators (name, next_id) VALUES ('inode', 1000);
INSERT IGNORE INTO id_allocators (name, next_id) VALUES ('block', 1000);
INSERT IGNORE INTO id_allocators (name, next_id) VALUES ('generation_stamp', 1000);
INSERT IGNORE INTO id_allocators (name, next_id) VALUES ('datanode', 1000);
INSERT IGNORE INTO id_allocators (name, next_id) VALUES ('lease', 1000);

-- ============================================================================
-- Operation Log: idempotency + audit trail
-- ============================================================================
CREATE TABLE IF NOT EXISTS op_log (
    id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    op_type         VARCHAR(64) NOT NULL,
    target_inode_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
    request_id      VARCHAR(128) NOT NULL,
    payload_json    TEXT,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_request_id (request_id),
    KEY idx_inode (target_inode_id)
) ENGINE=InnoDB;
