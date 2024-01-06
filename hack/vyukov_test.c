#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

#include "../src/include/mpmcq.h"

int main() {
    mpmcq queue;
    mpmcq_init(&queue, 1024);
    
    int arr[100];
    for (int i = 0; i < 100; i++) {
        arr[i] = i;
        mpmcq_enqueue(&queue, &arr[i]);
        printf("%d\n", arr[i]);
    }

    printf("done enqueueing\n");

    size_t count = mpmcq_count(&queue);
    printf("count: %zu\n", count);

    for (int i = 0; i < 100; i++) {
        int* data = mpmcq_dequeue(&queue);
        if (data == NULL) {
            printf("NULL\n");
            break;
        }
        printf("%d\n", *data);

        size_t count = mpmcq_count(&queue);
        printf("count: %zu\n", count);
    }

    return 0;
}