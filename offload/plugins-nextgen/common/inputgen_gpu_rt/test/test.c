#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>

typedef struct {
    int   id;
    float value;
    int   data[4];
} MyObject;

void copy_object_gpu(const MyObject *src, MyObject *dst);

int main(void) {
    MyObject src;
    src.id = 42;
    src.value = 3.14f;
    for (int i = 0; i < 4; i++) src.data[i] = i * i;

    MyObject dst;
    memset(&dst, 0, sizeof(dst));

    copy_object_gpu(&src, &dst);

    printf("[inputgen_gpu_rt test] single object copy\n");
    printf("dst.id    = %d\n", dst.id);
    printf("dst.value = %f\n", dst.value);
    printf("dst.data  = ");
    for (int i = 0; i < 4; i++) printf("%d ", dst.data[i]);
    printf("\n");

    return 0;
}

void copy_object_gpu(const MyObject *src, MyObject *dst) {
    MyObject s = *src;
    MyObject d;

    //#pragma omp target map(to: s) map(from: d)
    //{
    //    d = s;
    //}

    // para evadir muchas cosas de openmp wrapping
    #pragma omp target teams ompx_bare num_teams(1) thread_limit(1) map(to: s) map(from: d)
    {
            d = s;
    }

    *dst = d;
}
