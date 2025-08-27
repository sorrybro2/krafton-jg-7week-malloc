/*
 * mm.c — Three-in-one malloc package (implicit FF, implicit NF, explicit FF)
 *
 * Overview
 * --------
 * This refactor cleanly separates the *allocation policy* from the *shared
 * heap mechanics*. At build time, select exactly one of these policies:
 *   1) POLICY_IMPLICIT_FF  — implicit free list + first-fit
 *   2) POLICY_IMPLICIT_NF  — implicit free list + next-fit
 *   3) POLICY_EXPLICIT_FF  — explicit free list (doubly linked) + first-fit
 *
 * Shared code (always used):
 *   - Heap initialization and extension (mm_init, extend_heap)
 *   - Block format, header/footer helpers, alignment, coalescing logic core
 *   - Allocation placement/splitting (place)
 *   - Public API: mm_malloc / mm_free / mm_realloc
 *
 * Policy-specific code (compiled conditionally):
 *   - find_fit(): scanning strategy (implicit FF or NF, or explicit list walk)
 *   - Free-list hooks (insert/remove) used only by explicit policy
 *   - Rover pointer used only by implicit next-fit
 *
 * Switching policy (default: explicit first-fit):
 *   #define ALLOC_POLICY POLICY_EXPLICIT_FF
 *   // Or pass -DALLOC_POLICY=POLICY_IMPLICIT_FF / POLICY_IMPLICIT_NF at compile time.
 *
 * Notes
 * ---- make mdriver command (컴파일 명령어)
 *
 * 가용 리스트 만드는 방식과 메모리 할당 정책에 따라 오브젝트 파일을 다르게 만듭니다!
 *
 * 암시적 가용 리스트 + first-fit 정책
 * make clean && make CFLAGS+=' -DALLOC_POLICY=POLICY_IMPLICIT_FF'
 * 암시적 가용 리스트 + next-fit 정책
 * make clean && make CFLAGS+=' -DALLOC_POLICY=POLICY_IMPLICIT_NF'
 * 명시적 가용 리스트 + first-fit 정책
 * make clean && make CFLAGS+=' -DALLOC_POLICY=POLICY_EXPLICIT_FF'
 * 분리 가용 리스트 + best-fit 정책
 * make clean && make CFLAGS+=' -DALLOC_POLICY=POLICY_SEGREGATED_BF'
 * 
 * 실행 : ./mdriver -V
 * ----
 * 
 * - Block layout: | header | payload ... | footer |
 *   header/footer store (size | alloc-bit). Size is multiple of 8.
 * - MIN_BLOCK is policy-aware:
 *     implicit  : 2*DSIZE (header+footer + min payload = 16B)
 *     explicit  : header+footer + 2 pointers in payload (≈ 24B on 64-bit)
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "mm.h"
#include "memlib.h"

/********************** Team Info (fill for your course) **********************/
team_t team = {
    /* Team name */
    "ateam",
    /* First member's full name */
    "Your Name",
    /* First member's email address */
    "email@example.com",
    /* Second member's full name (leave blank if none) */
    "",
    /* Second member's email address (leave blank if none) */
    ""
};

/*************************** Global configuration ****************************/
/* single word (4) or double word (8) alignment */
#define ALIGNMENT 8

/* rounds up to the nearest multiple of ALIGNMENT */
#define ALIGN(size)     (((size) + (ALIGNMENT - 1)) & ~0x7) // 항상 8의 배수로 맞춤
#define SIZE_T_SIZE     (ALIGN(sizeof(size_t)))

#define WSIZE 4                 /* word size (bytes) - 1워드 = 4B */
#define DSIZE 8                 /* double word size (bytes) - 2워드 = 더블워드 = 8B */
#define CHUNKSIZE (1 << 12)     /* heap extension (bytes) - 2^12 = 4096 = 대략 4KB : 최소 힙 확장 크기 */

#define PACK(size, alloc)   ((unsigned)((size) | (alloc))) // 각 데이터 블럭 당 헤더 - size와 alloc 비트를 합쳐서 헤더/푸터에 저장
#define GET(p)              (*(unsigned *)(p)) // 주소 읽기 - 포인터 p가 가리키는 워드 반환
#define PUT(p, val)         (*(unsigned *)(p) = (val)) // 주소값에 해당 데이터 블럭 넣기 - 포인터 p가 가리키는 워드에 val 저장

#define GET_SIZE(p)         (GET(p) & ~0x7) // header와 footer에서 size 추출 - 하위 3비트 제거하여 크기만 반환
#define GET_ALLOC(p)        (GET(p) & 0x1) // header와 footer에서 alloc 추출 - 최하위 비트로 할당 여부 확인

#define HDRP(bp)            ((char *)(bp) - WSIZE) // 블록 포인터로부터 header 위치 계산 - payload 시작에서 한 워드 뒤로
#define FTRP(bp)            ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE) // 블록 포인터로부터 footer 위치 계산 - payload에서 블록 크기만큼 이동 후 더블워드 뒤로

#define NEXT_BLKP(bp)       ((char *)(bp) + GET_SIZE(HDRP(bp))) // 다음 블록의 payload 시작 주소 - 현재 블록 크기만큼 이동
#define PREV_BLKP(bp)       ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE))) // 현재 블록 바로 앞에 있는 블록의 payload 시작 주소 - 이전 블록의 푸터에서 크기를 읽어 역산

#define MAX(x,y)            ((x) > (y) ? (x) : (y)) // 둘이 비교해서 큰거 반환

/* ------------------------- Policy selection macros ------------------------- */
#define POLICY_IMPLICIT_FF 1 // 암시적 가용 리스트 + first-fit 정책
#define POLICY_IMPLICIT_NF 2 // 암시적 가용 리스트 + next-fit 정책  
#define POLICY_EXPLICIT_FF 3 // 명시적 가용 리스트 + first-fit 정책
#define POLICY_SEGREGATED_BF 4 // 분리 가용 리스트 + best-fit 정책

#ifndef ALLOC_POLICY
#define ALLOC_POLICY POLICY_EXPLICIT_FF // 기본값: 명시적 first-fit
#endif

/* Common forward declarations */
static void *extend_heap(size_t words); // 힙을 words만큼 확장하여 가용블록으로 초기화
static void *coalesce(void *bp); // 인접한 가용 블록들과 병합
static void *find_fit(size_t asize); // policy-specific - 적합한 가용 블록 찾기
static void place(void *bp, size_t asize); // 블록에 요청 크기만큼 할당하고 나머지는 분할

static char *heap_listp = NULL; /* prologue payload ptr - 프롤로그 블록의 payload 포인터 */

/* -------------------- Explicit free list (policy hooks) -------------------- */
#if ALLOC_POLICY == POLICY_EXPLICIT_FF
#  define PTRSIZE              (sizeof(void *)) // 포인터 크기 (보통 8바이트)
#  define PREV_FREEP(bp)       (*(char **)(bp)) // 가용 블록 payload의 첫 번째 포인터 - 이전 가용 블록 주소
#  define NEXT_FREEP(bp)       (*(char **)((char *)(bp) + PTRSIZE)) // 가용 블록 payload의 두 번째 포인터 - 다음 가용 블록 주소
#  define SET_PREV(bp, p)      (PREV_FREEP(bp) = (char *)(p)) // 이전 가용 블록 포인터 설정
#  define SET_NEXT(bp, p)      (NEXT_FREEP(bp) = (char *)(p)) // 다음 가용 블록 포인터 설정
static char *free_listp = NULL;       /* head of explicit free list - 명시적 가용 리스트의 머리 포인터 */
#endif

/* --------------------- Next-fit rover (policy hook) ------------------------ */
#if ALLOC_POLICY == POLICY_IMPLICIT_NF
static char *rover = NULL;            /* next-fit rover - next-fit용 탐색 시작 지점 포인터 */
#endif

/* ------------------- Segregated free lists (policy hooks) ------------------- */
#if ALLOC_POLICY == POLICY_SEGREGATED_BF
#  define PTRSIZE              (sizeof(void *)) // 포인터 크기 (보통 8바이트)
#  define PREV_FREEP(bp)       (*(char **)(bp)) // 가용 블록 payload의 첫 번째 포인터 - 이전 가용 블록 주소
#  define NEXT_FREEP(bp)       (*(char **)((char *)(bp) + PTRSIZE)) // 가용 블록 payload의 두 번째 포인터 - 다음 가용 블록 주소
#  define SET_PREV(bp, p)      (PREV_FREEP(bp) = (char *)(p)) // 이전 가용 블록 포인터 설정
#  define SET_NEXT(bp, p)      (NEXT_FREEP(bp) = (char *)(p)) // 다음 가용 블록 포인터 설정

#  define SEGREGATED_CLASSES   10 // 분리 리스트 개수 (크기 클래스별)
static char *segregated_lists[SEGREGATED_CLASSES]; /* 크기별 분리 가용 리스트 배열 */

// 크기 클래스 경계값들 (2^4=16, 2^5=32, 2^6=64, ..., 2^13=8192, 그 이상)
// Class 0: 16-31B, Class 1: 32-63B, ..., Class 9: 8192B+
#  define SIZE_CLASS_0_MIN     16
#  define SIZE_CLASS_0_MAX     31
#  define SIZE_CLASS_1_MIN     32
#  define SIZE_CLASS_1_MAX     63
#  define SIZE_CLASS_2_MIN     64
#  define SIZE_CLASS_2_MAX     127
#  define SIZE_CLASS_3_MIN     128
#  define SIZE_CLASS_3_MAX     255
#  define SIZE_CLASS_4_MIN     256
#  define SIZE_CLASS_4_MAX     511
#  define SIZE_CLASS_5_MIN     512
#  define SIZE_CLASS_5_MAX     1023
#  define SIZE_CLASS_6_MIN     1024
#  define SIZE_CLASS_6_MAX     2047
#  define SIZE_CLASS_7_MIN     2048
#  define SIZE_CLASS_7_MAX     4095
#  define SIZE_CLASS_8_MIN     4096
#  define SIZE_CLASS_8_MAX     8191
// Class 9: 8192B 이상
#endif

/* ---------------------- MIN_BLOCK depends on policy ------------------------ */
#if ALLOC_POLICY == POLICY_EXPLICIT_FF || ALLOC_POLICY == POLICY_SEGREGATED_BF
#  ifndef PTRSIZE
#    define PTRSIZE (sizeof(void *))
#  endif
#  define MIN_BLOCK ALIGN(WSIZE /*hdr*/ + 2*PTRSIZE /*prev,next*/ + WSIZE /*ftr*/) // 명시적/분리: 헤더(4) + 이전포인터(8) + 다음포인터(8) + 푸터(4) = 대략 24B
#else
#  define MIN_BLOCK (2*DSIZE) // 암시적: 헤더(4) + 푸터(4) + 최소 payload(8) = 16B
#endif

/****************************** mm_init / extend ******************************/
/*
 * mm_init - initialize the malloc package. 말록 패키지 초기화
 * 힙을 초기화하고 프롤로그/에필로그 블록을 생성한다.
 */
int mm_init(void)
{
    // 프롤로그와 에필로그를 포함한 최초 힙 생성 (4워드 = 16바이트)
    if ((heap_listp = mem_sbrk(4*WSIZE)) == (void *)-1)
        return -1; // 힙 확장 실패시 -1 반환

    PUT(heap_listp, 0);                         /* alignment padding - 정렬을 위한 패딩 */
    PUT(heap_listp + (1*WSIZE), PACK(DSIZE, 1));/* prologue header - 프롤로그 헤더 (크기:8, 할당됨) */
    PUT(heap_listp + (2*WSIZE), PACK(DSIZE, 1));/* prologue footer - 프롤로그 푸터 (크기:8, 할당됨) */
    PUT(heap_listp + (3*WSIZE), PACK(0, 1));    /* epilogue header - 에필로그 헤더 (크기:0, 할당됨) */
    heap_listp += (2*WSIZE); // 힙 리스트 포인터를 프롤로그 블록의 payload로 이동

#if ALLOC_POLICY == POLICY_EXPLICIT_FF
    free_listp = NULL; // 명시적 가용 리스트 초기화 - 빈 리스트로 시작
#elif ALLOC_POLICY == POLICY_SEGREGATED_BF
    // 분리 가용 리스트 초기화 - 모든 크기 클래스를 빈 리스트로 시작
    for (int i = 0; i < SEGREGATED_CLASSES; i++) {
        segregated_lists[i] = NULL;
    }
#endif
#if ALLOC_POLICY == POLICY_IMPLICIT_NF
    rover = heap_listp; // next-fit용 rover를 힙 시작점으로 초기화
#endif

    // 초기 가용 블록 생성을 위해 힙 확장 (CHUNKSIZE/WSIZE = 1024워드)
    if (extend_heap(CHUNKSIZE/WSIZE) == NULL)
        return -1; // 확장 실패시 -1 반환
    return 0; // 초기화 성공
}

/*
 * extend_heap - 힙을 words만큼 확장하여 새로운 가용블록 생성
 * 요청된 크기만큼 힙을 확장하고 새 가용 블록의 헤더/푸터를 설정한다.
 */
static void *extend_heap(size_t words)
{
    char *bp; // 새로 확장된 블록의 시작주소를 가리키는 포인터
    size_t size = (words % 2) ? (words+1)*WSIZE : words*WSIZE; /* keep 8-byte alignment - 8바이트 정렬을 위해 홀수면 +1 */
    
    if ((bp = mem_sbrk(size)) == (void *)-1) // 힙 확장 요청
        return NULL; // 확장 실패시 NULL 반환

    PUT(HDRP(bp), PACK(size, 0));              /* free block header - 새 가용 블록 헤더 설정 */
    PUT(FTRP(bp), PACK(size, 0));              /* free block footer - 새 가용 블록 푸터 설정 */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));      /* new epilogue - 새로운 에필로그 헤더 설정 */

    return coalesce(bp); // 이전 블록이 가용이면 병합 후 반환
}

/*************************** Policy: free-list ops ****************************/
#if ALLOC_POLICY == POLICY_EXPLICIT_FF
/*
 * insert_free_block - 가용 리스트에 새 블록 추가 (LIFO 방식)
 * 새로운 가용 블록을 리스트의 맨 앞(head)에 삽입한다.
 */
static void insert_free_block(void *bp)
{
    SET_PREV(bp, NULL); // 새로 넣을 노드 bp가 head가 될 것이므로 이전 노드는 NULL
    SET_NEXT(bp, free_listp); // 새 head의 next는 기존 head(free_listp)를 가리킴

    if (free_listp) SET_PREV(free_listp, bp); // 기존 head가 있다면 그것의 prev를 bp로 설정
    free_listp = (char *)bp; // 헤드 포인터를 bp로 갱신하여 bp가 새 head가 됨
}

/*
 * remove_free_block - 가용 리스트에서 블록 제거
 * 지정된 블록을 가용 리스트에서 제거하고 앞뒤 연결을 수정한다.
 */
static void remove_free_block(void *bp)
{
    char *prev = PREV_FREEP(bp); // 제거할 블록의 이전 블록 주소
    char *next = NEXT_FREEP(bp); // 제거할 블록의 다음 블록 주소

    // bp의 이전 블록이 있다면 그것의 next를 bp의 next로 연결
    // 없다면 (bp가 head였다면) free_listp를 bp의 next로 변경 (head 교체)
    if (prev) SET_NEXT(prev, next); 
    else      free_listp = next;
    
    // bp의 다음 블록이 있다면 그것의 prev를 bp의 prev로 연결
    if (next) SET_PREV(next, prev);
}
#endif

/******************* Segregated free list management *******************/
#if ALLOC_POLICY == POLICY_SEGREGATED_BF
/*
 * get_size_class - 블록 크기에 해당하는 분리 리스트 클래스 번호 반환
 * 크기에 따라 0~9 클래스로 분류한다.
 */
static int get_size_class(size_t size)
{
    // 이진 로그를 이용한 효율적인 클래스 결정
    if (size < 32) return 0;        // 16-31B
    if (size < 64) return 1;        // 32-63B  
    if (size < 128) return 2;       // 64-127B
    if (size < 256) return 3;       // 128-255B
    if (size < 512) return 4;       // 256-511B
    if (size < 1024) return 5;      // 512-1023B
    if (size < 2048) return 6;      // 1024-2047B
    if (size < 4096) return 7;      // 2048-4095B
    if (size < 8192) return 8;      // 4096-8191B
    return 9;                       // 8192B+
}

/*
 * insert_segregated_block - 해당 크기 클래스의 분리 리스트에 블록 추가 (LIFO 방식)
 * 크기에 맞는 분리 리스트의 맨 앞에 삽입한다.
 */
static void insert_segregated_block(void *bp)
{
    size_t size = GET_SIZE(HDRP(bp));
    int class = get_size_class(size);
    
    SET_PREV(bp, NULL); // 새로 넣을 노드가 head가 될 것이므로 이전 노드는 NULL
    SET_NEXT(bp, segregated_lists[class]); // 새 head의 next는 기존 head를 가리킴

    if (segregated_lists[class]) SET_PREV(segregated_lists[class], bp); // 기존 head가 있다면 그것의 prev를 bp로 설정
    segregated_lists[class] = (char *)bp; // 헤드 포인터를 bp로 갱신하여 bp가 새 head가 됨
}

/*
 * remove_segregated_block - 해당 크기 클래스의 분리 리스트에서 블록 제거
 * 지정된 블록을 분리 리스트에서 제거하고 앞뒤 연결을 수정한다.
 */
static void remove_segregated_block(void *bp)
{
    size_t size = GET_SIZE(HDRP(bp));
    int class = get_size_class(size);
    
    char *prev = PREV_FREEP(bp); // 제거할 블록의 이전 블록 주소
    char *next = NEXT_FREEP(bp); // 제거할 블록의 다음 블록 주소

    // bp의 이전 블록이 있다면 그것의 next를 bp의 next로 연결
    // 없다면 (bp가 head였다면) 해당 클래스의 head를 bp의 next로 변경
    if (prev) SET_NEXT(prev, next); 
    else      segregated_lists[class] = next;
    
    // bp의 다음 블록이 있다면 그것의 prev를 bp의 prev로 연결
    if (next) SET_PREV(next, prev);
}
#endif

/********************************* coalesce ***********************************/
/*
 * coalesce - 인접한 가용 블록들과 현재 블록을 병합
 * 4가지 경우를 고려하여 병합: 앞뒤 모두 할당/앞만 할당/뒤만 할당/앞뒤 모두 가용
 */
static void *coalesce(void *bp)
{
    // 이전 블록의 할당 상태 확인 - 이전 블록의 푸터에서 alloc 비트 읽기
    unsigned prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    // 다음 블록의 할당 상태 확인 - 다음 블록의 헤더에서 alloc 비트 읽기
    unsigned next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    // 현재 블록의 크기 - 헤더에서 size 필드 추출
    size_t size = GET_SIZE(HDRP(bp));

#if ALLOC_POLICY == POLICY_EXPLICIT_FF
    if (prev_alloc && next_alloc) { // Case 1: 이전과 다음 블록이 모두 할당됨 - 병합 불가
        insert_free_block(bp); // 현재 블록만 가용 리스트에 추가
        return bp;
    } else if (prev_alloc && !next_alloc) { // Case 2: 이전 블록은 할당, 다음 블록은 가용 - 다음과 병합
        void *next = NEXT_BLKP(bp); // 다음 블록 포인터
        remove_free_block(next); // 다음 블록을 가용 리스트에서 제거
        size += GET_SIZE(HDRP(next)); // 현재 블록 크기에 다음 블록 크기 추가
        PUT(HDRP(bp), PACK(size, 0)); // 병합된 블록의 헤더 설정 (현재 위치)
        PUT(FTRP(bp), PACK(size, 0)); // 병합된 블록의 푸터 설정 (다음 블록 위치)
        insert_free_block(bp); // 병합된 블록을 가용 리스트에 추가
        return bp;
    } else if (!prev_alloc && next_alloc) { // Case 3: 이전 블록은 가용, 다음 블록은 할당 - 이전과 병합
        void *prev = PREV_BLKP(bp); // 이전 블록 포인터
        remove_free_block(prev); // 이전 블록을 가용 리스트에서 제거
        size += GET_SIZE(HDRP(prev)); // 현재 블록 크기에 이전 블록 크기 추가
        PUT(FTRP(bp), PACK(size, 0)); // 병합된 블록의 푸터 설정 (현재 위치)
        PUT(HDRP(prev), PACK(size, 0)); // 병합된 블록의 헤더 설정 (이전 블록 위치)
        insert_free_block(prev); // 병합된 블록을 가용 리스트에 추가
        return prev;
    } else { // Case 4: 이전과 다음 블록이 모두 가용 - 삼중 병합
        void *prev = PREV_BLKP(bp); // 이전 블록 포인터
        void *next = NEXT_BLKP(bp); // 다음 블록 포인터
        remove_free_block(prev); // 이전 블록을 가용 리스트에서 제거
        remove_free_block(next); // 다음 블록을 가용 리스트에서 제거
        size += GET_SIZE(HDRP(prev)) + GET_SIZE(HDRP(next)); // 세 블록의 크기 모두 합산
        PUT(HDRP(prev), PACK(size, 0)); // 병합된 블록의 헤더 설정 (이전 블록 위치)
        PUT(FTRP(next), PACK(size, 0)); // 병합된 블록의 푸터 설정 (다음 블록 위치)
        insert_free_block(prev); // 병합된 블록을 가용 리스트에 추가
        return prev;
    }
#elif ALLOC_POLICY == POLICY_SEGREGATED_BF
    if (prev_alloc && next_alloc) { // Case 1: 이전과 다음 블록이 모두 할당됨 - 병합 불가
        insert_segregated_block(bp); // 현재 블록만 해당 크기 클래스 리스트에 추가
        return bp;
    } else if (prev_alloc && !next_alloc) { // Case 2: 이전 블록은 할당, 다음 블록은 가용 - 다음과 병합
        void *next = NEXT_BLKP(bp); // 다음 블록 포인터
        remove_segregated_block(next); // 다음 블록을 해당 크기 클래스 리스트에서 제거
        size += GET_SIZE(HDRP(next)); // 현재 블록 크기에 다음 블록 크기 추가
        PUT(HDRP(bp), PACK(size, 0)); // 병합된 블록의 헤더 설정 (현재 위치)
        PUT(FTRP(bp), PACK(size, 0)); // 병합된 블록의 푸터 설정 (다음 블록 위치)
        insert_segregated_block(bp); // 병합된 블록을 새로운 크기에 맞는 클래스 리스트에 추가
        return bp;
    } else if (!prev_alloc && next_alloc) { // Case 3: 이전 블록은 가용, 다음 블록은 할당 - 이전과 병합
        void *prev = PREV_BLKP(bp); // 이전 블록 포인터
        remove_segregated_block(prev); // 이전 블록을 해당 크기 클래스 리스트에서 제거
        size += GET_SIZE(HDRP(prev)); // 현재 블록 크기에 이전 블록 크기 추가
        PUT(FTRP(bp), PACK(size, 0)); // 병합된 블록의 푸터 설정 (현재 위치)
        PUT(HDRP(prev), PACK(size, 0)); // 병합된 블록의 헤더 설정 (이전 블록 위치)
        insert_segregated_block(prev); // 병합된 블록을 새로운 크기에 맞는 클래스 리스트에 추가
        return prev; // 병합 후 시작점은 이전 블록
    } else { // Case 4: 이전과 다음 블록이 모두 가용 - 삼중 병합
        void *prev = PREV_BLKP(bp); // 이전 블록 포인터
        void *next = NEXT_BLKP(bp); // 다음 블록 포인터
        remove_segregated_block(prev); // 이전 블록을 해당 크기 클래스 리스트에서 제거
        remove_segregated_block(next); // 다음 블록을 해당 크기 클래스 리스트에서 제거
        size += GET_SIZE(HDRP(prev)) + GET_SIZE(HDRP(next)); // 세 블록의 크기 모두 합산
        PUT(HDRP(prev), PACK(size, 0)); // 병합된 블록의 헤더 설정 (이전 블록 위치)
        PUT(FTRP(next), PACK(size, 0)); // 병합된 블록의 푸터 설정 (다음 블록 위치)
        insert_segregated_block(prev); // 병합된 블록을 새로운 크기에 맞는 클래스 리스트에 추가
        return prev; // 병합 후 시작점은 이전 블록
    }
#else /* IMPLICIT (FF or NF) - 암시적 가용 리스트의 경우 */
    if (prev_alloc && next_alloc) { // Case 1: 이전과 다음 블록이 모두 할당됨 - 병합 불가
        return bp; // 현재 블록 그대로 반환
    } else if (prev_alloc && !next_alloc) { // Case 2: 이전 블록은 할당, 다음 블록은 가용 - 다음과 병합
        size += GET_SIZE(HDRP(NEXT_BLKP(bp))); // 현재 블록 크기에 다음 블록 크기 추가
        PUT(HDRP(bp), PACK(size, 0)); // 병합된 블록의 헤더 설정 (현재 위치)
        PUT(FTRP(bp), PACK(size, 0)); // 병합된 블록의 푸터 설정 (다음 블록 끝)
#if ALLOC_POLICY == POLICY_IMPLICIT_NF
        // next-fit: rover가 병합되는 영역에 있었다면 새 블록 시작점으로 이동
        if ((char *)rover >= bp && (char *)rover <= NEXT_BLKP(bp)) {
            rover = bp;
        }
#endif
        return bp;
    } else if (!prev_alloc && next_alloc) { // Case 3: 이전 블록은 가용, 다음 블록은 할당 - 이전과 병합
        size += GET_SIZE(HDRP(PREV_BLKP(bp))); // 현재 블록 크기에 이전 블록 크기 추가
        PUT(FTRP(bp), PACK(size, 0)); // 병합된 블록의 푸터 설정 (현재 위치)
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0)); // 병합된 블록의 헤더 설정 (이전 블록 위치)
#if ALLOC_POLICY == POLICY_IMPLICIT_NF
        // next-fit: rover가 병합되는 영역에 있었다면 새 블록 시작점으로 이동
        if ((char *)rover >= (char *)PREV_BLKP(bp) && (char *)rover <= (char *)bp) {
            rover = PREV_BLKP(bp);
        }
#endif
        return PREV_BLKP(bp); // 병합 후 시작점은 이전 블록
    } else { // Case 4: 이전과 다음 블록이 모두 가용 - 삼중 병합
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(HDRP(NEXT_BLKP(bp))); // 세 블록의 크기 모두 합산
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0)); // 병합된 블록의 헤더 설정 (이전 블록 위치)
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0)); // 병합된 블록의 푸터 설정 (다음 블록 위치)
#if ALLOC_POLICY == POLICY_IMPLICIT_NF
        // next-fit: rover가 병합되는 영역에 있었다면 새 블록 시작점으로 이동
        if ((char *)rover >= (char *)PREV_BLKP(bp) && (char *)rover <= (char *)NEXT_BLKP(bp)) {
            rover = PREV_BLKP(bp);
        }
#endif
        return PREV_BLKP(bp); // 병합 후 시작점은 이전 블록
    }
#endif
}

/******************************** find_fit ************************************/
/*
 * find_fit - 적합한 가용 블록 찾기 (정책에 따라 다른 구현)
 * 요청된 크기 이상의 가용 블록을 찾아 반환한다.
 */
static void *find_fit(size_t asize)
{
#if ALLOC_POLICY == POLICY_EXPLICIT_FF
    // 명시적 first-fit: 가용 리스트를 처음부터 순회하며 첫 번째 적합한 블록 반환
    for (char *bp = free_listp; bp != NULL; bp = NEXT_FREEP(bp)) {
        if (GET_SIZE(HDRP(bp)) >= asize) return bp; // 요청 크기 이상이면 즉시 반환
    }
    return NULL; // 적합한 블록 없음
#elif ALLOC_POLICY == POLICY_IMPLICIT_NF
    // 암시적 next-fit: rover 위치부터 힙 끝까지 탐색
    char *bp;
    // 첫 번째 탐색: rover에서 힙 끝까지
    for (bp = rover; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
        // 가용블록이고 요청 크기 이상이면 적합
        if (!GET_ALLOC(HDRP(bp)) && GET_SIZE(HDRP(bp)) >= asize) {
            rover = bp; // 찾은 위치를 rover에 기록하여 다음 탐색 시작점으로 설정
            return bp;
        }
    }
    // 두 번째 탐색: 힙 시작에서 rover 이전까지 (순환 탐색)
    for (bp = heap_listp; bp < rover; bp = NEXT_BLKP(bp)) {
        // 가용블록이고 요청 크기 이상이면 적합
        if (!GET_ALLOC(HDRP(bp)) && GET_SIZE(HDRP(bp)) >= asize) {
            rover = bp; // 찾은 위치를 rover에 기록하여 다음 탐색 시작점으로 설정
            return bp;
        }
    }
    return NULL; // 적합한 블록 없음
#elif ALLOC_POLICY == POLICY_SEGREGATED_BF
    // 분리 가용 리스트 + best-fit: 해당 크기 클래스부터 시작해서 best-fit 탐색
    int start_class = get_size_class(asize);
    void *best_bp = NULL;
    size_t best_size = SIZE_MAX;
    
    // 요청 크기에 맞는 클래스부터 시작해서 상위 클래스들을 순회
    for (int class = start_class; class < SEGREGATED_CLASSES; class++) {
        // 해당 클래스의 리스트를 순회하며 best-fit 찾기
        for (char *bp = segregated_lists[class]; bp != NULL; bp = NEXT_FREEP(bp)) {
            size_t block_size = GET_SIZE(HDRP(bp));
            if (block_size >= asize) {
                // 현재까지 찾은 best보다 더 적합한(작은) 블록이면 업데이트
                if (block_size < best_size) {
                    best_bp = bp;
                    best_size = block_size;
                    // 정확히 맞는 크기를 찾았으면 즉시 반환 (perfect fit)
                    if (best_size == asize) return best_bp;
                }
            }
        }
        // 해당 클래스에서 적합한 블록을 찾았으면 반환 (하위 클래스에서 찾는 것이 더 효율적)
        if (best_bp) return best_bp;
    }
    return NULL; // 적합한 블록 없음
#else /* POLICY_IMPLICIT_FF */
    // 암시적 first-fit: 힙 시작부터 순회하며 첫 번째 적합한 블록 반환
    for (char *bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
        // 가용블록이고 요청 크기 이상이면 적합
        if (!GET_ALLOC(HDRP(bp)) && GET_SIZE(HDRP(bp)) >= asize) return bp;
    }
    return NULL; // 적합한 블록 없음
#endif
}

/********************************** place *************************************/
/*
 * place - 블록에 요청 크기만큼 할당하고 필요시 분할
 * 찾은 가용 블록에 요청된 크기를 할당하고, 남는 공간이 충분하면 새 가용 블록으로 분할한다.
 */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp)); // 현재 가용 블록의 전체 크기

#if ALLOC_POLICY == POLICY_EXPLICIT_FF
    remove_free_block(bp); // 명시적: 할당하기 전에 가용 리스트에서 제거
#elif ALLOC_POLICY == POLICY_SEGREGATED_BF
    remove_segregated_block(bp); // 분리: 할당하기 전에 해당 클래스 리스트에서 제거
#endif

    // 할당 후 남는 공간이 최소 블록 크기 이상이면 분할
    if (csize - asize >= MIN_BLOCK) {
        /* allocate front part - 앞 부분을 요청 크기로 할당 */
        PUT(HDRP(bp), PACK(asize, 1)); // 할당 블록의 헤더 설정
        PUT(FTRP(bp), PACK(asize, 1)); // 할당 블록의 푸터 설정

        /* create a new free block with the remainder - 남는 부분으로 새 가용 블록 생성 */
        void *nbp = NEXT_BLKP(bp); // 분할된 새 블록의 시작 위치
        size_t rem = csize - asize; // 남는 크기 계산
        PUT(HDRP(nbp), PACK(rem, 0)); // 새 가용 블록의 헤더 설정
        PUT(FTRP(nbp), PACK(rem, 0)); // 새 가용 블록의 푸터 설정

#if ALLOC_POLICY == POLICY_EXPLICIT_FF
        insert_free_block(nbp); // 명시적: 새 가용 블록을 리스트에 추가
#elif ALLOC_POLICY == POLICY_SEGREGATED_BF
        insert_segregated_block(nbp); // 분리: 새 가용 블록을 해당 클래스 리스트에 추가
#elif ALLOC_POLICY == POLICY_IMPLICIT_NF
        rover = nbp; // next-fit: 분할된 가용 블록을 다음 탐색 시작점으로 설정
#endif
    } else {
        /* consume entire block - 블록 전체를 할당 (분할하지 않음) */
        PUT(HDRP(bp), PACK(csize, 1)); // 전체 블록 할당으로 헤더 설정
        PUT(FTRP(bp), PACK(csize, 1)); // 전체 블록 할당으로 푸터 설정
#if ALLOC_POLICY == POLICY_IMPLICIT_NF
        rover = NEXT_BLKP(bp); // next-fit: 할당된 블록 다음을 탐색 시작점으로 설정
#endif
    }
}

/********************************* API: malloc ********************************/
/*
 * mm_malloc - 요청 크기만큼 메모리 블록 할당
 * 8바이트 정렬된 블록을 할당하고, 적합한 블록이 없으면 힙을 확장한다.
 */
void *mm_malloc(size_t size)
{
    if (size == 0) return NULL; // 0 바이트 요청시 NULL 반환

    // 요청 크기에 헤더/푸터 오버헤드 추가하고 8바이트 정렬
    size_t asize = ALIGN(size + DSIZE);      /* add overhead and align - 헤더(4)+푸터(4) 8B 오버헤드 추가 후 8의 배수로 정렬 */
    if (asize < MIN_BLOCK) asize = MIN_BLOCK; /* enforce policy minimum - 정책별 최소 블록 크기 보장 */

    // 적합한 가용 블록 탐색
    void *bp = find_fit(asize);
    if (bp) { // 적합한 블록을 찾았다면
        place(bp, asize); // 블록에 할당하고 필요시 분할
        return bp; // 할당된 블록의 payload 포인터 반환
    }

    // 적합한 블록이 없으면 힙 확장
    size_t extendsize = MAX(asize, CHUNKSIZE); // 요청 크기와 기본 확장 크기 중 큰 값
    bp = extend_heap(extendsize/WSIZE); // 워드 단위로 힙 확장
    if (bp == NULL) return NULL; // 확장 실패시 NULL 반환
    place(bp, asize); // 확장된 블록에 할당
    return bp; // 할당된 블록의 payload 포인터 반환
}

/********************************** API: free *********************************/
/*
 * mm_free - 할당된 블록을 해제하고 인접 가용 블록과 병합
 * 지정된 포인터의 블록을 가용 상태로 만들고 coalesce를 통해 병합한다.
 */
void mm_free(void *ptr)
{
    if (ptr == NULL) return; // NULL 포인터는 무시
    size_t size = GET_SIZE(HDRP(ptr)); // 해제할 블록의 크기 확인
    PUT(HDRP(ptr), PACK(size, 0)); // 헤더를 가용 상태로 변경
    PUT(FTRP(ptr), PACK(size, 0)); // 푸터를 가용 상태로 변경
    (void)coalesce(ptr); // 인접 가용 블록들과 병합
}

/******************************** API: realloc ********************************/
/*
 * mm_realloc - 기존 블록의 크기를 변경
 * 가능하면 제자리에서 확장/축소하고, 불가능하면 새로 할당 후 복사한다.
 */
void *mm_realloc(void *ptr, size_t size)
{
    // 예외 처리
    if (ptr == NULL) return mm_malloc(size); // NULL 포인터면 새로 할당
    if (size == 0) { mm_free(ptr); return NULL; } // 크기 0이면 해제

    // 요청 크기 정렬 및 최소 블록 크기 보장
    size_t asize = ALIGN(size + DSIZE); // 헤더/푸터 포함하여 8의 배수로 정렬
    if (asize < MIN_BLOCK) asize = MIN_BLOCK; // 정책별 최소 블록 크기 적용

    size_t csize = GET_SIZE(HDRP(ptr)); // 현재 블록의 크기

    // Case 1: 축소 - 요청 크기가 현재 크기보다 작거나 같음
    if (asize <= csize) {
        size_t excess = csize - asize; // 축소 후 남는 크기
        if (excess >= MIN_BLOCK) { // 남는 공간이 최소 블록 크기 이상이면 분할
            PUT(HDRP(ptr), PACK(asize, 1)); // 축소된 블록의 헤더 설정
            PUT(FTRP(ptr), PACK(asize, 1)); // 축소된 블록의 푸터 설정
            void *split = NEXT_BLKP(ptr); // 분할될 블록의 시작 위치
            PUT(HDRP(split), PACK(excess, 0)); // 분할된 가용 블록의 헤더 설정
            PUT(FTRP(split), PACK(excess, 0)); // 분할된 가용 블록의 푸터 설정
            (void)coalesce(split); // 분할된 블록을 인접 가용 블록과 병합
        }
        // 남는 공간이 작으면 내부 단편화 허용하고 분할하지 않음
        return ptr; // 기존 포인터 반환
    }

    // Case 2: 확장 시도 - 다음 블록이 가용이면 병합하여 제자리 확장
    void *next = NEXT_BLKP(ptr); // 다음 블록 포인터
    if (!GET_ALLOC(HDRP(next))) { // 다음 블록이 가용 상태라면
        size_t combined = csize + GET_SIZE(HDRP(next)); // 현재 + 다음 블록의 총 크기
        if (combined >= asize) { // 병합 후 크기가 요청 크기 이상이면
#if ALLOC_POLICY == POLICY_EXPLICIT_FF
            remove_free_block(next); // 명시적: 다음 블록을 가용 리스트에서 제거
#elif ALLOC_POLICY == POLICY_SEGREGATED_BF
            remove_segregated_block(next); // 분리: 다음 블록을 해당 클래스 리스트에서 제거
#endif
            PUT(HDRP(ptr), PACK(combined, 1)); // 병합된 블록으로 헤더 설정
            PUT(FTRP(ptr), PACK(combined, 1)); // 병합된 블록으로 푸터 설정
            
            // 병합 후에도 남는 공간이 있으면 분할
            size_t rem = combined - asize; // 병합 후 남는 크기
            if (rem >= MIN_BLOCK) { // 남는 공간이 최소 블록 크기 이상이면
                PUT(HDRP(ptr), PACK(asize, 1)); // 요청 크기로 블록 조정
                PUT(FTRP(ptr), PACK(asize, 1));
                void *split = NEXT_BLKP(ptr); // 분할될 블록 위치
                PUT(HDRP(split), PACK(rem, 0)); // 남는 부분을 가용 블록으로 설정
                PUT(FTRP(split), PACK(rem, 0));
                (void)coalesce(split); // 분할된 블록 병합
            }
            return ptr; /* grown in place - 제자리 확장 성공 */
        }
    }

    // Case 3: 제자리 확장 불가 - 새로 할당 후 데이터 복사
    void *newp = mm_malloc(size); // 새 블록 할당
    if (newp == NULL) return NULL; // 할당 실패시 NULL 반환
    
    size_t copySize = csize - DSIZE; /* payload only - 헤더/푸터 제외한 payload 크기 */
    if (size < copySize) copySize = size; // 복사할 크기는 요청 크기와 기존 payload 중 작은 값
    memcpy(newp, ptr, copySize); // 기존 데이터를 새 블록으로 복사
    mm_free(ptr); // 기존 블록 해제
    return newp; // 새 블록 포인터 반환
}

/****************************** End of mm.c ***********************************/
