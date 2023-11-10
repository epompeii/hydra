/*-------------------------------------------------------------------------
 *
 * columnar.h
 *
 * Type and function declarations for Columnar
 *
 * Copyright (c) Citus Data, Inc.
 * Copyright (c) Hydra, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLUMNAR_H
#define COLUMNAR_H
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/parsenodes.h"
#include "nodes/extensible.h"
#include "port/atomics.h"
#include "storage/bufpage.h"
#include "storage/lockdefs.h"
#if PG_VERSION_NUM >= PG_VERSION_16
#include "storage/relfilelocator.h"
#else
#include "storage/relfilenode.h"
typedef struct RelFileNode RelFileLocator;
#endif
#include "storage/s_lock.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"

#include "columnar/columnar_compression.h"
#include "columnar/columnar_metadata.h"
#include "columnar/columnar_write_state_row_mask.h"

#define COLUMNAR_MODULE_NAME "citus_columnar"

#define COLUMNAR_SETOPTIONS_HOOK_SYM "ColumnarTableSetOptions_hook"

/* Defines for valid option names */
#define OPTION_NAME_COMPRESSION_TYPE "compression"
#define OPTION_NAME_STRIPE_ROW_COUNT "stripe_row_limit"
#define OPTION_NAME_CHUNK_ROW_COUNT "chunk_group_row_limit"

/* Limits for option parameters */
#define STRIPE_ROW_COUNT_MINIMUM 1000
#define STRIPE_ROW_COUNT_MAXIMUM 100000000
#define CHUNK_ROW_COUNT_MINIMUM 1000
#define CHUNK_ROW_COUNT_MAXIMUM 100000000
#define COMPRESSION_LEVEL_MIN 1
#define COMPRESSION_LEVEL_MAX 19

/* Columnar file signature */
#define COLUMNAR_VERSION_MAJOR 2
#define COLUMNAR_VERSION_MINOR 0

/* miscellaneous defines */
#define COLUMNAR_TUPLE_COST_MULTIPLIER 10
#define COLUMNAR_POSTSCRIPT_SIZE_LENGTH 1
#define COLUMNAR_POSTSCRIPT_SIZE_MAX 256
#define COLUMNAR_BYTES_PER_PAGE (BLCKSZ - SizeOfPageHeaderData)

/* Columnar row mask byte array size */
#define COLUMNAR_ROW_MASK_CHUNK_SIZE 10000

/*
 * ColumnarOptions holds the option values to be used when reading or writing
 * a columnar table. To resolve these values, we first check foreign table's options,
 * and if not present, we then fall back to the default values specified above.
 */
typedef struct ColumnarOptions
{
	uint64 stripeRowCount;
	uint32 chunkRowCount;
	CompressionType compressionType;
	int compressionLevel;
} ColumnarOptions;


/* ColumnChunkSkipNode contains statistics for a ColumnChunkData. */
typedef struct ColumnChunkSkipNode
{
	/* statistics about values of a column chunk */
	bool hasMinMax;
	Datum minimumValue;
	Datum maximumValue;
	uint64 rowCount;

	/*
	 * Offsets and sizes of value and exists streams in the column data.
	 * These enable us to skip reading suppressed row chunks, and start reading
	 * a chunk without reading previous chunks.
	 */
	uint64 valueChunkOffset;
	uint64 valueLength;
	uint64 existsChunkOffset;
	uint64 existsLength;

	/*
	 * This is used for (1) determining destination size when decompressing,
	 * (2) calculating compression rates when logging stats.
	 */
	uint64 decompressedValueSize;

	CompressionType valueCompressionType;
	int valueCompressionLevel;
} ColumnChunkSkipNode;


/*
 * StripeSkipList can be used for skipping row chunks. It contains a column chunk
 * skip node for each chunk of each column. chunkSkipNodeArray[column][chunk]
 * is the entry for the specified column chunk.
 */
typedef struct StripeSkipList
{
	ColumnChunkSkipNode **chunkSkipNodeArray;
	uint32 *chunkGroupRowCounts;
	uint32 *chunkGroupRowOffset;
	uint32 *chunkGroupDeletedRows;
	uint32 columnCount;
	uint32 chunkCount;
} StripeSkipList;


/*
 * ChunkData represents a chunk of data for multiple columns. valueArray stores
 * the values of data, and existsArray stores whether a value is present.
 * valueBuffer is used to store (uncompressed) serialized values
 * referenced by Datum's in valueArray. It is only used for by-reference Datum's.
 * There is a one-to-one correspondence between valueArray and existsArray.
 */
typedef struct ChunkData
{
	uint32 rowCount;
	uint32 columnCount;

	/*
	 * Following are indexed by [column][row]. If a column is not projected,
	 * then existsArray[column] and valueArray[column] are NULL.
	 */
	bool **existsArray;
	Datum **valueArray;

	/* valueBuffer keeps actual data for type-by-reference datums from valueArray. */
	StringInfo *valueBufferArray;
} ChunkData;


/*
 * ColumnChunkBuffers represents a chunk of serialized data in a column.
 * valueBuffer stores the serialized values of data, and existsBuffer stores
 * serialized value of presence information. valueCompressionType contains
 * compression type if valueBuffer is compressed. Finally rowCount has
 * the number of rows in this chunk.
 */
typedef struct ColumnChunkBuffers
{
	StringInfo existsBuffer;
	StringInfo valueBuffer;
	CompressionType valueCompressionType;
	uint64 decompressedValueSize;
} ColumnChunkBuffers;


/*
 * ColumnBuffers represents data buffers for a column in a row stripe. Each
 * column is made of multiple column chunks.
 */
typedef struct ColumnBuffers
{
	ColumnChunkBuffers **chunkBuffersArray;
} ColumnBuffers;


/* StripeBuffers represents data for a row stripe. */
typedef struct StripeBuffers
{
	uint32 columnCount;
	uint32 rowCount;
	ColumnBuffers **columnBuffersArray;

	uint32 *selectedChunkGroupRowCounts;
	uint32 *selectedChunkGroupRowOffset;
	uint32 *selectedChunkGroupDeletedRows;
} StripeBuffers;


/* return value of StripeWriteState to decide stripe write state */
typedef enum StripeWriteStateEnum
{
	/* stripe write is flushed to disk, so it's readable */
	STRIPE_WRITE_FLUSHED,

	/*
	 * Writer transaction did abort either before inserting into
	 * columnar.stripe or after.
	 */
	STRIPE_WRITE_ABORTED,

	/*
	 * Writer transaction is still in-progress. Note that it is not certain
	 * if it is being written by current backend's current transaction or
	 * another backend.
	 */
	STRIPE_WRITE_IN_PROGRESS
} StripeWriteStateEnum;


/* Parallel Custom Scan shared data */
typedef struct ParallelColumnarScanData
{
	slock_t mutex;
	pg_atomic_uint64 nextStripeId;	/* Fetch next stripe id to be read and increment */
	char snapshotData[FLEXIBLE_ARRAY_MEMBER];
} ParallelColumnarScanData;
typedef struct ParallelColumnarScanData *ParallelColumnarScan;


typedef bool (*ColumnarSupportsIndexAM_type)(char *);
typedef const char *(*CompressionTypeStr_type)(CompressionType);
typedef bool (*IsColumnarTableAmTable_type)(Oid);
typedef bool (*ReadColumnarOptions_type)(Oid, ColumnarOptions *);

/* ColumnarReadState represents state of a columnar scan. */
struct ColumnarReadState;
typedef struct ColumnarReadState ColumnarReadState;


/* ColumnarWriteState represents state of a columnar write operation. */
struct ColumnarWriteState;
typedef struct ColumnarWriteState ColumnarWriteState;

/* RowMaskWriteStateEntry represents write state entry of columnar.row_mask row */
struct RowMaskWriteStateEntry;
typedef struct RowMaskWriteStateEntry RowMaskWriteStateEntry;

/* Cache statistics for when caching is enabled and used. */
typedef struct ColumnarCacheStatistics
{
	uint64 hits;
	uint64 misses;
	uint64 evictions;
	uint64 writes;
	uint64 maximumCacheSize;
	uint64 endingCacheSize;
	uint64 entries;
} ColumnarCacheStatistics;

/* GUCs */
extern int columnar_compression;
extern int columnar_stripe_row_limit;
extern int columnar_chunk_group_row_limit;
extern int columnar_compression_level;
extern bool columnar_enable_parallel_execution;
extern int columnar_min_parallel_processes;
extern bool columnar_enable_vectorization;
extern bool columnar_enable_dml;
extern bool columnar_enable_page_cache;
extern int columnar_page_cache_size;


/* called when the user changes options on the given relation */
typedef void (*ColumnarTableSetOptions_hook_type)(Oid relid, ColumnarOptions options);

extern void columnar_init(void);

extern CompressionType ParseCompressionType(const char *compressionTypeString);

/* Function declarations for writing to a columnar table */
extern ColumnarWriteState * ColumnarBeginWrite(RelFileLocator relfilelocator,
											   ColumnarOptions options,
											   TupleDesc tupleDescriptor);
extern uint64 ColumnarWriteRow(ColumnarWriteState *state, Datum *columnValues,
							   bool *columnNulls);
extern void ColumnarFlushPendingWrites(ColumnarWriteState *state);
extern void ColumnarEndWrite(ColumnarWriteState *state);
extern bool ContainsPendingWrites(ColumnarWriteState *state);
extern MemoryContext ColumnarWritePerTupleContext(ColumnarWriteState *state);

/* Function declarations for reading from columnar table */

/* functions applicable for both sequential and random access */
extern ColumnarReadState * ColumnarBeginRead(Relation relation,
											 TupleDesc tupleDescriptor,
											 List *projectedColumnList,
											 List *qualConditions,
											 MemoryContext scanContext,
											 Snapshot snaphot,
											 bool randomAccess,
											 ParallelColumnarScan parallaleColumnarScan);
extern void ColumnarReadFlushPendingWrites(ColumnarReadState *readState);
extern void ColumnarEndRead(ColumnarReadState *state);
extern void ColumnarResetRead(ColumnarReadState *readState);

/* functions only applicable for sequential access */
extern bool ColumnarReadNextRow(ColumnarReadState *state, Datum *columnValues,
								bool *columnNulls, uint64 *rowNumber);
extern bool ColumnarReadNextVector(ColumnarReadState *readState, Datum *columnValues,
								   bool *columnNulls, uint64 *rowNumber,
								   int *newVectorSize);
extern int64 ColumnarReadChunkGroupsFiltered(ColumnarReadState *state);
extern void ColumnarRescan(ColumnarReadState *readState, List *scanQual);

/* functions only applicable for random access */
extern void ColumnarReadRowByRowNumberOrError(ColumnarReadState *readState,
											  uint64 rowNumber, Datum *columnValues,
											  bool *columnNulls);
extern bool ColumnarReadRowByRowNumber(ColumnarReadState *readState,
									   uint64 rowNumber, Datum *columnValues,
									   bool *columnNulls);
extern bool ColumnarSetStripeReadState(ColumnarReadState *readState,
									   StripeMetadata *startStripeMetadata);

/* Function declarations for common functions */
extern FmgrInfo * GetFunctionInfoOrNull(Oid typeId, Oid accessMethodId,
										int16 procedureId);
extern ChunkData * CreateEmptyChunkData(uint32 columnCount, bool *columnMask,
										uint32 chunkGroupRowCount);
extern void FreeChunkData(ChunkData *chunkData);
extern void FreeChunkBufferValueArray(ChunkData *chunkData);
extern uint64 ColumnarTableRowCount(Relation relation);
extern const char * CompressionTypeStr(CompressionType type);
extern ItemPointerData row_number_to_tid(uint64 rowNumber);
extern uint64 LookupStorageId(RelFileLocator relfilelocator);

/* columnar_metadata_tables.c */
extern void InitColumnarOptions(Oid regclass);
extern void SetColumnarOptions(Oid regclass, ColumnarOptions *options);
extern bool DeleteColumnarTableOptions(Oid regclass, bool missingOk);
extern bool ReadColumnarOptions(Oid regclass, ColumnarOptions *options);
extern bool IsColumnarTableAmTable(Oid relationId);

/* columnar_metadata_tables.c */
extern void DeleteMetadataRows(RelFileLocator relfilelocator);
extern void DeleteMetadataRowsForStripeId(RelFileLocator relfilelocator, uint64 stripeId);
extern uint64 ColumnarMetadataNewStorageId(void);
extern uint64 GetHighestUsedAddress(RelFileLocator relfilelocator);
extern EmptyStripeReservation * ReserveEmptyStripe(Relation rel, uint64 columnCount,
												   uint64 chunkGroupRowCount,
												   uint64 stripeRowCount);
extern StripeMetadata * CompleteStripeReservation(Relation rel, uint64 stripeId,
												  uint64 sizeBytes, uint64 rowCount,
												  uint64 chunkCount);
extern void SaveStripeSkipList(RelFileLocator relfilelocator, uint64 stripe,
							   StripeSkipList *stripeSkipList,
							   TupleDesc tupleDescriptor);
extern void SaveChunkGroups(RelFileLocator relfilelocator, uint64 stripe,
							List *chunkGroupRowCounts);
extern void UpdateChunkGroupDeletedRows(uint64 storageId, uint64 stripe,
										uint32 chunkGroupId, uint32 deletedRowNumber);
extern StripeSkipList * ReadStripeSkipList(RelFileLocator relfilelocator, uint64 stripe,
										   TupleDesc tupleDescriptor,
										   uint32 chunkCount,
										   Snapshot snapshot);
extern StripeMetadata * FindNextStripeByRowNumber(Relation relation, uint64 rowNumber,
												  Snapshot snapshot);
extern StripeMetadata * FindStripeByRowNumber(Relation relation, uint64 rowNumber,
											  Snapshot snapshot);
extern StripeMetadata * FindStripeWithMatchingFirstRowNumber(Relation relation,
															 uint64 rowNumber,
															 Snapshot snapshot);
extern StripeMetadata *  FindNextStripeForParallelWorker(Relation relation,
														 Snapshot snapshot,
														 uint64 nextStripeId,
														 uint64 * nextHigherStripeId);
extern StripeWriteStateEnum StripeWriteState(StripeMetadata *stripeMetadata);
extern uint64 StripeGetHighestRowNumber(StripeMetadata *stripeMetadata);
extern StripeMetadata * FindStripeWithHighestRowNumber(Relation relation,
													   Snapshot snapshot);
extern Datum columnar_relation_storageid(PG_FUNCTION_ARGS);
extern bool SaveEmptyRowMask(uint64 storageId, uint64 stripeId,
							 uint64 stripeStartRowNumber, List *chunkGroupRowCounts);
extern bool UpdateRowMask(RelFileLocator relfilelocator, uint64 storageId,
						  Snapshot snapshot, uint64 rowNumber);
extern void FlushRowMaskCache(RowMaskWriteStateEntry *rowMaskEntry);
extern bytea * ReadChunkRowMask(RelFileLocator relfilelocator, Snapshot snapshot,
								MemoryContext ctx,
								uint64 stripeFirstRowNumber, int rowCount);
extern Datum create_table_row_mask(PG_FUNCTION_ARGS);
extern EState * create_estate_for_relation(Relation rel);

/* columnar_planner_hook.c */
extern void columnar_planner_init(void);

/* write_state_interface.c */
extern void FlushWriteStateWithNewSnapshot(Oid relfilelocator,
										   Snapshot * snapshot,
										   bool * snapShotRegisteredByUs);
extern void FlushWriteStateForAllRels(SubTransactionId currentSubXid,
									  SubTransactionId parentSubXid);
extern void DiscardWriteStateForAllRels(SubTransactionId currentSubXid,
										SubTransactionId parentSubXid);
extern void MarkRelfilenodeDropped(Oid relfilelocator, SubTransactionId currentSubXid);
extern void NonTransactionDropWriteState(Oid relfilelocator);
extern bool PendingWritesInUpperTransactions(Oid relfilelocator,
											 SubTransactionId currentSubXid);

/* write_state_management.c */
extern ColumnarWriteState * columnar_init_write_state(Relation relation, TupleDesc
													  tupdesc,
														Oid tupSlotRelationId,
													  SubTransactionId currentSubXid);
extern void ColumnarPopWriteStateForAllRels(SubTransactionId currentSubXid,
											SubTransactionId parentSubXid,
											bool commit);
extern void ColumnarMarkRelfilenodeDroppedColumnar(Oid relfilelocator,
												   SubTransactionId currentSubXid);
extern void ColumnarNonTransactionDropWriteState(Oid relfilelocator);
extern bool ColumnarPendingWritesInUpperTransactions(Oid relfilelocator,
													 SubTransactionId currentSubXid);
extern void FlushWriteStateForRelfilenode(Oid relfilelocator, SubTransactionId
										  currentSubXid);
extern MemoryContext GetColumnarWriteContextForDebug(void);

/* write_state_row_mask.c */
extern RowMaskWriteStateEntry * RowMaskInitWriteState(Oid relfilelocator,
													  uint64 storageId,
													  SubTransactionId currentSubXid,
							 						  bytea *rowMask);
extern void RowMaskFlushWriteStateForRelfilenode(Oid relfilelocator, 
												 SubTransactionId currentSubXid);
extern RowMaskWriteStateEntry * RowMaskFindWriteState(Oid relfilelocator,
					  								  SubTransactionId currentSubXid,
													  uint64 rowId);
extern void  RowMaskPopWriteStateForAllRels(SubTransactionId currentSubXid,
											SubTransactionId parentSubXid,
											bool commit);
extern void RowMaskMarkRelfilenodeDropped(Oid relfilelocator,
										  SubTransactionId currentSubXid);
extern void RowMaskNonTransactionDrop(Oid relfilelocator);
extern bool RowMaskPendingWritesInUpperTransactions(Oid relfilelocator,
													SubTransactionId currentSubXid);
extern MemoryContext GetRowMaskWriteStateContextForDebug(void);

/* columanar_read_state_cache.c */
extern ColumnarReadState ** InitColumnarReadStateCache(Relation relation,
													  SubTransactionId currentSubXid);
extern ColumnarReadState ** FindReadStateCache(Relation relation,
											   SubTransactionId currentSubXid);
extern void CleanupReadStateCache(SubTransactionId currentSubXid);
extern MemoryContext GetColumnarReadStateCache(void);

/* columnar_cache.c */
extern void ColumnarMarkChunkGroupInUse(uint64 relId, uint64 stripeId, uint32 chunkId);
extern void ColumnarAddCacheEntry(uint64, uint64, uint64, uint32, void *);
extern void *ColumnarRetrieveCache(uint64, uint64, uint64, uint32);
extern void ColumnarResetCache(void);
extern ColumnarCacheStatistics *ColumnarGetCacheStatistics(void);
extern MemoryContext ColumnarCacheMemoryContext(void);


#endif /* COLUMNAR_H */
