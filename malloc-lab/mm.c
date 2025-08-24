/*
 * mm-naive.c - The fastest, least memory-efficient malloc package.
 *
 * In this naive approach, a block is allocated by simply incrementing
 * the brk pointer.  A block is pure payload. There are no headers or
 * footers.  Blocks are never coalesced or reused. Realloc is
 * implemented directly using mm_malloc and mm_free.
 *
 * NOTE TO STUDENTS: Replace this header comment with your own header
 * comment that gives a high level description of your solution.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

/*********************************************************
 * NOTE TO STUDENTS: Before you do anything else, please
 * provide your team information in the following struct.
 ********************************************************/
team_t team = {
    /* Team name */
    "ateam",
    /* First member's full name */
    "Harry Bovik",
    /* First member's email address */
    "bovik@cs.cmu.edu",
    /* Second member's full name (leave blank if none) */
    "",
    /* Second member's email address (leave blank if none) */
    ""};

/* single word (4) or double word (8) alignment */
#define ALIGNMENT 8

/* rounds up to the nearest multiple of ALIGNMENT */
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~0x7)

#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))

/*
 * mm_init - initialize the malloc package. 초기 설정
 */

#define WSIZE 4 // 1워드 = 4B
#define DSIZE 8 // 2워드 = 더블워드 = 8B
#define CHUNKSIZE (1 << 12) //2^12 최소 힙 확장 크기

#define PACK(size, alloc) ((unsigned)((size)|(alloc))) // 각 데이터 블럭 당 헤더
#define PUT(p, val) (*(unsigned*)(p) = (val)) // 주소값에 해당 데이터 블럭 넣기
#define GET(p) (*(unsigned*)(p)) // 주소 읽기

#define GET_SIZE(p) (GET(p) & ~0x7) // header와 footer에서 size 추출
#define GET_ALLOC(p) (GET(p) & 0x1) // header와 footer에서 alloc 추출

#define HDRP(bp) ((char *)(bp) - WSIZE) // 블록 포인터로부터 header 위치 계산
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE) // 블록 포인터로부터 footer 위치 계산

#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE((char *)(bp) - WSIZE)) // 다음 블록의 payload 시작 주소
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE((char))) // 현재 블록 바로 앞에 있는 블록의 payload 시작 주소

#define ALIGN(size) (((size) + (DSIZE-1)) & ~0x7) // 항상 8의 배수로 맞춤
#define MAX(x, y) ((x) > (y) ? (x) : (y)) // 둘이 비교해서 큰거
#define MIN_BLOCK (2*DSIZE) // (header+footer)(8) + 최소 payload(8) = 16B 

static char *heap_listp = NULL; // 실질적 주소는 프롤로그 블록이므로 

int mm_init(void)
{
    PUT(heap_listp, 0); // padding (4B)
    PUT(heap_listp + (WSIZE), PACK(DSIZE, 1)); // 에필로그 헤더 (8/alloc)
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); // 에필로그 풋터 (8/alloc)
    PUT(heap_listp + (3 * WSIZE), PACK(0,1)); // 프롤로그 헤더 (0/alloc)
    heap_listp += (2 * WSIZE); // 페이로드 시작 지점

    if(extend_heap(heap_listp / WSIZE) ==  NULL){ // 확장되는 힙이 8배수 정렬 유지가 안됐다면
        return -1;
    }

    return 0;
}

static void *extend_heap(size_t words) // 힙을 워드만큼 확장하여 가용블록으로 초기화
{
    char *bp; // 새로 확장된 블록의 시작주소를 가리키는 포인터 생성
    size_t size; // 실제로 저장되는 바이트 사이즈

    size = (words % 2) ? (words+1) * WSIZE : words * WSIZE; // 정렬 유지 8배수를 유지하기 위해 

    if ((bp = mem_sbrk(size)) == (void *)-1){ // 힙 확장 : 실패 시 (void *)-1 -> NULL 반환
        return NULL;
    }

    // 새로 생긴 가용 블록의 헤더 풋터 가용 데이터이므로 alloc = 0
    PUT(HDRP(bp), PACK(size, 0)); // new free header
    PUT(FTRP(bp), PACK(size, 0)); // new free footer

    // 새 에필로그 헤더 (size = 0, alloc = 1)
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); // new epligue

    // 인접 free와 병합
    return coalesce(bp);
}

// 인접 free와 새로 추가된 가용 블록과 병합
static void coalesce(void *bp)
{
    // 이전 블록의 alloc 할당을 탐색하므로 PREV_BLKP 이전 블록의 payload에 위치한 FDRP 푸터를 검색
    unsigned prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    // 다음 블록의 alloc 할당을 탐색하므로 NEXT_BLKP 다음 블록의 payload에 위치한 HDRP 헤더를 검색
    unsigned next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    // 헤더에 블록 전체 크기 + alloc 비트가 저장 되어있으므로 size만 추출해서 사용
    size_t size = GET_SIZE(HDRP(bp));

    // free 가용 블럭인 경우에만 병합 과정이 이뤄져야한다! (경계 태그 병합 4가지 경우)
    if (prev_alloc && next_alloc){ // 1. 이전과 다음 블록이 할당되어 있을 때 -> 병합 x
        return bp; // 
    }else if (prev_alloc && !next_alloc){ // 2. 이전 블록은 할당, 다음 블록은 가용 -> 뒤와 병합
        size += GET_SIZE(HDRP(NEXT_BLKP(bp))); // 현재 블록 크기(size)에 뒤에 블록 크기 추가
        PUT(HDRP(bp), PACK(size, 0)); // 헤더 최신화 (현재 위치 유지)
        PUT(FTRP(bp), PACK(size, 0)); // 푸터 최신화 (뒤 블록 위치) -> 뒤와 병합했기 때문!
    }else if (!prev_alloc && next_alloc){ // 3. 다음 블록은 할당, 이전 블록이 가용 -> 앞과 병합
        size += GET_SIZE(HDRP(PREV_BLKP(bp))); // 현재 블록 크기에 앞에 블록 크기 추가
        PUT(FTRP(bp), PACK(size, 0)); // 푸터 최신화 (현재 위치 유지)
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0)); // 헤더 최신화 (앞 블록 위치)
        bp = PREV_BLKP(bp); // 앞에 붙였으므로 시작점 포인터를 앞에 블록으로 바꿈
    }else{ // 4. 이전 다음 블록 모두 가용 -> 모두 병합
        size += GET_SIZE(HDRP(NEXT_BLKP(bp))) + GET_SIZE(HDRP(PREV_BLKP(bp))); // 현재 블록 크기에 앞+뒤 블록 크기 추가
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0)); // 헤더 최신화 (앞 블록 위치)
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0)); // 푸터 최신화 (뒤 블록 위치)
        bp = PREV_BLKP(bp); // 시작 포인터 전 블록으로 바꿈
    }
    return bp; //합병 완!
}


/*
 * mm_malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */

void *mm_malloc(size_t size)
{
    int newsize = ALIGN(size + SIZE_T_SIZE);
    void *p = mem_sbrk(newsize);
    if (p == (void *)-1)
        return NULL;
    else
    {
        *(size_t *)p = size;
        return (void *)((char *)p + SIZE_T_SIZE);
    }
}

/*
 * mm_free - Freeing a block does nothing.
 */
void mm_free(void *ptr)
{
    size_t size = GET_SIZE(HDRP(ptr)); // 해당 위치에 있는 사이즈만 빼옴
    PUT(HDRP(ptr), PACK(size, 0)); // 
    PUT(FTRP(ptr), PACK(size, 0));
    coalesce(ptr);
}

/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */
void *mm_realloc(void *ptr, size_t size)
{
    void *oldptr = ptr;
    void *newptr;
    size_t copySize;

    newptr = mm_malloc(size);
    if (newptr == NULL)
        return NULL;
    copySize = *(size_t *)((char *)oldptr - SIZE_T_SIZE);
    if (size < copySize)
        copySize = size;
    memcpy(newptr, oldptr, copySize);
    mm_free(oldptr);
    return newptr;
}