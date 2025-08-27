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
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~0x7) // 항상 8의 배수로 맞춤

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
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE((char *)(bp) - DSIZE)) // 현재 블록 바로 앞에 있는 블록의 payload 시작 주소

#define MAX(x, y) ((x) > (y) ? (x) : (y)) // 둘이 비교해서 큰거

//implicit
// #define MIN_BLOCK (2*DSIZE) // (header+footer)(8) + 최소 payload(8) = 16B 

// explicit
#define MIN_BLOCK ALIGN(WSIZE + 2*PTRSIZE + WSIZE) // 대략 24B

static char *heap_listp = NULL; // 프롤로그 payLoad
// static char *rover = NULL; // next-fit용 탐색 시작 지점! 

/* ==== 명시적 가용 리스트 포인터(페이로드 안 2칸 사용) ==== */
#define PTRSIZE             (sizeof(void *))
#define PREV_FREEP(bp)      (*(char **)(bp))
#define NEXT_FREEP(bp)      (*(char **)((char *)(bp) + PTRSIZE))
#define SET_PREV(bp, p)     (PREV_FREEP(bp) = (char *)(p))
#define SET_NEXT(bp, p)     (NEXT_FREEP(bp) = (char *)(p))

//명시적
static char *free_listp = NULL; // 가용 리스트 머리(Head)

/* ===== 함수 원형 선언 ===== */
static void *extend_heap(size_t words);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void place(void *bp, size_t asize);

// 명시적
static void insert_free_block(void *bp); // LIFO(head 삽입)
static void remove_free_block(void *bp); // 리스트에서 제거


int mm_init(void)
{
    // 프롤로그 에필로그 포함 16B 공간 확보 <- heap_listp 초기화
    if ((heap_listp = mem_sbrk(4*WSIZE)) == (void *)-1){
        return -1;
    }

    PUT(heap_listp, 0); // padding (4B)
    PUT(heap_listp + (WSIZE), PACK(DSIZE, 1)); // 프롤로그 헤더 (8/alloc)
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); // 프롤로그 풋터 (8/alloc)
    PUT(heap_listp + (3 * WSIZE), PACK(0,1)); // 에필로그 헤더 (0/alloc)
    heap_listp += (2 * WSIZE); // 페이로드 시작 지점

    // 명시적 가용 리스트 비움
    free_listp = NULL;

    if((extend_heap(CHUNKSIZE / WSIZE)) ==  NULL){ // 확장되는 힙이 8배수 정렬 유지가 안됐다면
        return -1;
    }

    // next-fit에서 사용할 rover 변수 생성
    // rover = heap_listp;
    return 0;
}

static void *extend_heap(size_t words) // 힙을 워드만큼 확장하여 가용블록으로 초기화
{
    char *bp; // 새로 확장된 블록의 시작주소를 가리키는 포인터 생성
    size_t size; // 실제로 저장되는 바이트 사이즈

    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE; // 정렬 8배수를 유지하기 위해 

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

/* ====== 명시적 가용 리스트 조작 함수 ====== */
// 가용 리스트 추가 함수
static void insert_free_block(void *bp)
{
    // 참고하셈
    // #define PREV_FREEP(bp)      (*(char **)(bp))
    // #define NEXT_FREEP(bp)      (*(char **)((char *)(bp) + PTRSIZE))
    // #define SET_PREV(bp, p)     (PREV_FREEP(bp) = (char *)(p))
    // #define SET_NEXT(bp, p)     (NEXT_FREEP(bp) = (char *)(p))


    //LIFO(head에 삽입)
    SET_PREV(bp, NULL); // 새로 넣을 노드 bp가 head가 될 것이므로 이전 노드가 없음. PREV에는 NULL.
    SET_NEXT(bp, free_listp); // 새 head의 next는 기존 head(= free_listp)를 가리킴.

    // HEAD 이후 더 넣는다면
    if (free_listp != NULL){ // free_listp에 내용물 있으면
        SET_PREV(free_listp, bp); // 기존 head의 prev를 bp로 설정
    }
    free_listp = (char *)bp; // 헤드 포인터를 bp로 갱신해서 bp가 새 head가 된다!
}

// 가용 리스트 제거 함수
static void remove_free_block(void *bp)
{
    // 해당 블록의 이전 포인트와 다음 포인트 변수 지정
    char *prev = PREV_FREEP(bp);
    char *next = NEXT_FREEP(bp);

    // bp의 왼쪽 이웃(prev)이 있다면, prev->next = next
    // 없다면 (bp가 head였다면), free_listp = next (head 교체)
    if (prev) SET_NEXT(prev, next);
    else    free_listp = next;
    if (next) SET_PREV(next, prev);
}

// 인접 free와 새로 추가된 가용 블록과 병합
static void *coalesce(void *bp)
{
    // 이전 블록의 alloc 할당을 탐색하므로 PREV_BLKP 이전 블록의 payload에 위치한 FDRP 푸터를 검색
    unsigned prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    // 다음 블록의 alloc 할당을 탐색하므로 NEXT_BLKP 다음 블록의 payload에 위치한 HDRP 헤더를 검색
    unsigned next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    // 헤더에 블록 전체 크기 + alloc 비트가 저장 되어있으므로 size만 추출해서 사용
    size_t size = GET_SIZE(HDRP(bp));

    // free 가용 블럭인 경우에만 병합 과정이 이뤄져야한다! (경계 태그 병합 4가지 경우)
    if (prev_alloc && next_alloc){ // 1. 이전과 다음 블록이 할당되어 있을 때 -> 병합 x
        // Implicit first, next_fit
        // return bp;

        // explicit
        // 양옆이 할당이면 합칠 필요가 없으므로 가용 리스트에 바로 쳐넣는다!
        insert_free_block(bp);

    }else if (prev_alloc && !next_alloc){ // 2. 이전 블록은 할당, 다음 블록은 가용 -> 뒤와 병합
        // next_fit : rover가 [bp, NEXT_BLKP(bp)] 사이였다면 결과 블록 시작으로
        // if ((char *)rover >= bp && (char *)rover <= NEXT_BLKP(bp)){
        //     rover = bp;
        // }

        // explicit
        // free 블록을 리스트에 제거하고 합친 후 더 큰 free를 삽입
        void *next = NEXT_BLKP(bp); // next 주소 블록 가져와!
        remove_free_block(next); // 제거해!

        size += GET_SIZE(HDRP(next)); // 현재 블록 크기(size)에 뒤에 블록 크기 추가
        PUT(HDRP(bp), PACK(size, 0)); // 헤더 최신화 (현재 위치 유지)
        PUT(FTRP(bp), PACK(size, 0)); // 푸터 최신화 (뒤 블록 위치) -> 뒤와 병합했기 때문!

        // explicit
        // 병합된 가용 블록 리스트에 넣기
        insert_free_block(bp);

    }else if (!prev_alloc && next_alloc){ // 3. 다음 블록은 할당, 이전 블록이 가용 -> 앞과 병합
        // next_fit : rover가 [PREV_BLKP(bp), bp] 사이였다면 앞 블록 시작으로
        // if ((char *)rover >= (char *)PREV_BLKP(bp) && (char *)rover <= (char *)bp){
        //     rover = PREV_BLKP(bp);
        // }

        // explicit
        // 앞과 병합
        void *prev = PREV_BLKP(bp); // prev 주소 블록 가져와!
        remove_free_block(prev); // 제거해!

        size += GET_SIZE(HDRP(prev)); // 합쳐
        PUT(HDRP(prev), PACK(size, 0)); // 합병한 주소인 prev로 헤더 최신화
        PUT(FTRP(prev), PACK(size, 0)); // 합병한 주소인 prev로 푸터 최신화

        bp = prev; // bp 주소도 prev로 바꿈
        insert_free_block(bp);

        // implicit
        // size += GET_SIZE(HDRP(PREV_BLKP(bp))); // 현재 블록 크기에 앞에 블록 크기 추가
        // PUT(FTRP(bp), PACK(size, 0)); // 푸터 최신화 (현재 위치 유지)
        // PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0)); // 헤더 최신화 (앞 블록 위치)
        // bp = PREV_BLKP(bp); // 앞에 붙였으므로 시작점 포인터를 앞에 블록으로 바꿈

    }else{ // 4. 이전 다음 블록 모두 가용 -> 모두 병합
        // next_fit : rover가 [PREV_BLKP(bp), NEXT_BLKP(bp)] 사이였다면 앞 블록 시작
        // if ((char *)bp >= (char *)PREV_BLKP(bp) && (char *)bp <= (char *)NEXT_BLKP(bp)){
        //     rover = PREV_BLKP(bp);
        // }

        void *prev = PREV_BLKP(bp); // 전 블록
        void *next = NEXT_BLKP(bp); // 다음 블록 변수 
        remove_free_block(prev); // 전 블록 제거
        remove_free_block(next); // 다음 블록 제거

        size += GET_SIZE(HDRP(prev)) + GET_SIZE(HDRP(next)); // 합쳐 
        PUT(HDRP(prev), PACK(size, 0)); // 헤더 최신화
        PUT(FTRP(prev), PACK(size, 0)); // 푸터 최신화

        bp = prev; // bp 주소 prev로 바꿈
        insert_free_block(bp);

        // implicit
        // size += GET_SIZE(HDRP(NEXT_BLKP(bp))) + GET_SIZE(HDRP(PREV_BLKP(bp))); // 현재 블록 크기에 앞+뒤 블록 크기 추가
        // PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0)); // 헤더 최신화 (앞 블록 위치)
        // PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0)); // 푸터 최신화 (뒤 블록 위치)
        // bp = PREV_BLKP(bp); // 시작 포인터 전 블록으로 바꿈
    }
    return bp; //합병 완!
}

// implicit
//first_fit find_fit 함수로 구현!
// static void *find_fit(size_t asize)
// {
//     // find_fit 안에서의 지역 변수
//     void *bp;

//     // 시작 : 전역변수 heap_listp 힙 시작 주소
//     // 끝 : 에필로그(size = 0) 만나면 종료
//     // 진행 : NEXT_BLKP(bp)로 한 블럭씩
//     for(bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)){
//         // 가용블록 (alloc = 0)이고 들어갈 메모리 크기가 넣는 메모리 크기보다 작거나 같아야 함
//         // 그래야 들어갈 첫 블럭을 찾지!
//         if (!GET_ALLOC(HDRP(bp)) && asize <= GET_SIZE(HDRP(bp))){
//             return bp;
//         }
//     }
//     return NULL; //no fit
// }

//next_fit find_fit 함수로 구현!
// static void *find_fit(size_t asize)
// {
//     void *bp;
//     // rover에서 힙 끝까지
//     for(bp = rover; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)){
//         if(!GET_ALLOC(HDRP(bp)) && asize <= GET_SIZE(HDRP(bp))){
//             rover = bp; // 찾은 위치를 rover에 기록
//             return bp;
//         }
//     }

//     // 힙 시작에서 rover까지
//     for(bp = heap_listp; bp < rover; bp = NEXT_BLKP(bp)){
//         if(!GET_ALLOC(HDRP(bp)) && asize <= GET_SIZE(HDRP(bp))){
//             rover = bp; // 찾은 위치를 rover에 기록
//             return bp;
//         }
//     }

//     return NULL; //no fit
// }

// explicit
// first fit
static void *find_fit(size_t asize){
    for (char *bp = free_listp; bp != NULL; bp = NEXT_FREEP(bp)){
        size_t bsize = GET_SIZE(HDRP(bp));
        if (bsize >= asize) return bp; //처음꺼에 바로 맞추기!
    }
    return NULL;
}

// first-fit-place함수
// static void place(void *bp, size_t asize)
// {
//     //할당할 가용 블록
//     size_t csize = GET_SIZE(HDRP(bp));

//     // 전체 크기에서 넣는 메모리 크기가 16b 이상일 때, 남는 메모리를 alloc과 free로 분할
//     if(csize - asize >= 2*DSIZE){
        
//         // asize 할당
//         PUT(HDRP(bp), PACK(asize, 1));
//         PUT(FTRP(bp), PACK(asize, 1));
        
//         // 다음 블럭으로 넘어감
//         bp = NEXT_BLKP(bp);

//         // 넘어간 블럭에 free 할당
//         PUT(HDRP(bp), PACK(csize-asize, 0));
//         PUT(FTRP(bp), PACK(csize-asize, 0));

//     // 16b 미만일 경우 헤더/푸터만으로 거의 다 먹거나 
//     // 다음 번 할당에 쓸 수 없는 쪼가리(spliter)가 되서 그냥 alloc만 함
//     }else{
//         PUT(HDRP(bp), PACK(csize, 1));
//         PUT(FTRP(bp), PACK(csize, 1));
//     }
// }

// next-fit-place함수
// static void place(void *bp, size_t asize)
// {
//     size_t csize = GET_SIZE(HDRP(bp));

//     if (csize - asize >= 2*DSIZE){

//         // 앞쪽에 asize 배치
//         PUT(HDRP(bp), PACK(asize, 1));
//         PUT(FTRP(bp), PACK(asize, 1));

//         // bp = NEXT_BLKP(bp); // first_fit
//         void *next = NEXT_BLKP(bp); // next_fit : 다음 블록 넘어가니까 next 포인터 변수 생성

//         // first_fit
//         // PUT(HDRP(bp), PACK(csize-asize, 0));
//         // PUT(FTRP(bp), PACK(csize-asize, 0));

//         // next_fit : 뒷 블록 free 분할
//         PUT(HDRP(next), PACK(csize-asize, 0));
//         PUT(FTRP(next), PACK(csize-asize, 0));

//         // next_fit : 남은 free 조각을 다음 탐색 시작점으로
//         rover = next;

//     }else{
//         PUT(HDRP(bp), PACK(csize, 1));
//         PUT(FTRP(bp), PACK(csize, 1));

//         // 할당 블록 다음 블록에 시작점 부여
//         rover = NEXT_BLKP(bp); 
//     }
// }

//explicit
//place = 할당
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));
    remove_free_block(bp); //가용 제거

    if (csize - asize >= MIN_BLOCK){ // 할당 구간에 남는 공간이 16b 이상이라면
        // 앞 부분은 asize 할당, 뒷 부분을 새 free 블록으로 남김 
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));

        void *nbp = NEXT_BLKP(bp); // 한 번에 계산 하기 위한 변수 설정
        size_t rem = csize - asize;
        PUT(HDRP(nbp), PACK(rem, 0));
        PUT(FTRP(nbp), PACK(rem, 0));
        insert_free_block(nbp);
    } else {
        // 16b 미만이면 모든 공간 할당
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

/*
 * mm_malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */

void *mm_malloc(size_t size)
{
    size_t asize; // 8의 배수로 조정할 블록 크기
    size_t extendsize; // 확장한 블록 크기
    char *bp;

    if(size == 0) return NULL; // 0 할당하면 당연히 NULL

    // 헤더(4B)+푸터(4B) 8B 오버헤드 결과를 8의 배수로 하되, 최소 크기는 16B
    //**오버헤드(overhead)**는 “실제 유저가 쓰는 데이터(payload) 외에, 동작을 관리·유지하기 위해 추가로 드는 비용”
    asize = ALIGN(size + DSIZE);
    if (asize < MIN_BLOCK) asize = MIN_BLOCK;

    // 적합 블럭 탐색
    // find_fit으로 들어갈 곳 탐색 (현재 방법 : first_fit)
    if ((bp = find_fit(asize)) != NULL){ // 만약에 들어갈 곳이 있으면
        place(bp, asize); // 적합한 블록에 asize만큼 할당
        return bp;
    }

    // 없으면 힙 확장 후 배치
    extendsize = MAX(asize, CHUNKSIZE); //최소 2^12 = 4KB
    if ((bp = extend_heap(extendsize/WSIZE)) == NULL){ // 추가적인 힙을 추가함
        return NULL; // 추가가 안되면 NULL 리턴
    }

    place(bp, asize); // 적합한 블록(방금 추가한 힙)에 asize만큼 할당
    return bp;
}

/*
 * mm_free - Freeing a block does nothing.
 */
void mm_free(void *ptr)
{
    size_t size = GET_SIZE(HDRP(ptr)); // 해당 위치에 있는 사이즈만 빼옴
    PUT(HDRP(ptr), PACK(size, 0)); // 가용 블럭 생성
    PUT(FTRP(ptr), PACK(size, 0));
    coalesce(ptr); // 앞뒤에 가용블럭이 있다면 가용 블럭끼리 합침
}

/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */

//implicit realloc(최적화 필요)
// void *mm_realloc(void *ptr, size_t size)
// {
//     void *oldptr = ptr;
//     void *newptr;
//     size_t copySize;

//     newptr = mm_malloc(size);
//     if (newptr == NULL)
//         return NULL;
//     copySize = *(size_t *)((char *)oldptr - SIZE_T_SIZE);
//     if (size < copySize)
//         copySize = size;
//     memcpy(newptr, oldptr, copySize);
//     mm_free(oldptr);
//     return newptr;
// }

//explicit realloc
void *mm_realloc(void *ptr, size_t size)
{
    // 0. 예외 처리
    // 지정 포인터에 아무것도 없으면 할당
    if (ptr == NULL) return mm_malloc(size);
    // size = 0으로 바꾸고 싶으면 사실 free임
    if (size == 0) {mm_free(ptr); return NULL;}

    // 1. 요청 크기 정렬 + 최소 블록 보장
    // header + footer 포함 정렬 
    size_t asize = ALIGN(size + DSIZE); // 8의 배수로 맞춤
    if (asize < MIN_BLOCK) asize = MIN_BLOCK; // 16 아래라면 최소값인 16으로 할당

    size_t csize = GET_SIZE(HDRP(ptr));

    // 2. 축소 : 남는 공간이 MIN_BLOCK(24B) 이상이면 뒤로 분할하고 free 리스트에 삽입
    //          작으면 분할하지 않고 현재 블록을 csize 그대로 유지(내부 단편화 허용)
    if (asize <= csize) {
        size_t excess = csize - asize; // 축소하고 남는 애
        if (excess >= MIN_BLOCK){ // excess이 한 블럭을 넘어갈 정도의 크기라면
            PUT(HDRP(ptr), PACK(asize, 1)); // 일단 ptr 헤더 푸터를 asize 크기를 할당으로 최신화
            PUT(FTRP(ptr), PACK(asize, 1));

            // 초과분을 다음 블록에도 담아야 함으로
            void *split = NEXT_BLKP(ptr); // 다음 주소 블록을 이동하는 split
            PUT(HDRP(split), PACK(excess, 0)); // split 주소에 excess 초과분 사이즈로 최신화
            PUT(FTRP(split), PACK(excess, 0)); // split 주소에 excess 초과분 사이즈로 최신화
            insert_free_block(split);
            coalesce(split);
        }
        return ptr;
    }

    // 3. 확장 : 바로 다음 물리 블록이 free면 in-place 확장 시도
    void *next = NEXT_BLKP(ptr); // 다음 블록 변수
    if (!GET_ALLOC(HDRP(next))){ // 다음 블록이 가용이라면
        size_t combined = csize + GET_SIZE(HDRP(next)); // 현재 블록 크기 + 다음 블록 크기 더함
        if (combined >= asize) { // 확장하고픈 크기가 확장한 크기보다 작거나 같다면
            remove_free_block(next); // 명시적 : 다음 블록을 가용 블럭에서 제거해주고

            // combined 된 크기로 최신화해서 일단 키워놓음
            PUT(HDRP(ptr), PACK(combined, 1));
            PUT(FTRP(ptr), PACK(combined, 1));

            // 할당 후 남은 크기는 다시 뒤로 분할하여 free로 전달
            size_t rem = combined - asize; // 남는 크기
            if (rem >= MIN_BLOCK){ // 남는 크기 24b 이상이면
                PUT(HDRP(ptr), PACK(asize, 1)); // ptr에 asize만큼 할당
                PUT(FTRP(ptr), PACK(asize, 1));

                // 남는 부분 다음 블록을 넘겨
                void *split = NEXT_BLKP(ptr);
                PUT(HDRP(split), PACK(rem, 0)); // 남는 부분을 rem 크기의 가용 블럭으로 
                PUT(FTRP(split), PACK(rem, 0));
                insert_free_block(split); // 남는 부분 가용 리스트에 넣기
            }

            return ptr; // 제자리 확장 성공
        }
    }

    // 4. 실패하면 새로 할당 후 복사
    void *newp = mm_malloc(size);
    if (newp == NULL) return NULL;

    size_t old_payload = csize - DSIZE; //헤더/푸터 8B를 제외
    size_t copySize = (size < old_payload) ? size : old_payload;
    memcpy(newp, ptr, copySize);
    mm_free(ptr);
    return newp;
}