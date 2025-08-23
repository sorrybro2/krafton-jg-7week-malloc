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

static char *heap_listp = NULL; // 실질적 주소는 프롤로그 블록이므로 

int mm_init(void)
{
    PUT(heap_listp, 0); // padding (4B)
    PUT(heap_listp + (WSIZE), PACK(DSIZE, 1)); // 에필로그 헤더 (8/alloc)
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); // 에필로그 풋터 (8/alloc)
    PUT(heap_listp + (3 * WSIZE), PACK(0,1)); // 프롤로그 헤더 (0/alloc)
    heap_listp += (2 * WSIZE); // 페이로드 시작 지점

    if(extend_heap(heap_listp / WSIZE) ==  NULL){
        return -1;
    }

    return 0;
}

static void *extend_heap(size_t words) // 힙을 워드만큼 확장하여 가용블록으로 초기화
{
    char *bp; // 
    size_t size;


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