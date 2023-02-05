#ifndef VECTOR_H
#define VECTOR_H
#include <stdio.h>
#include <stdlib.h>

#define VECTOR_TYPE(T)                                                         \
  typedef struct {                                                             \
    T *array;                                                                  \
    size_t used;                                                               \
    size_t size;                                                               \
  } vector_##T;                                                                \
                                                                               \
  void init_vector_##T(vector_##T *a, size_t initialSize) {                    \
    a->array = (T *)malloc(initialSize * sizeof(T));                           \
    a->used = 0;                                                               \
    a->size = initialSize;                                                     \
  }                                                                            \
                                                                               \
  void insert_vector_##T(vector_##T *a, T element) {                           \
    if (a->used == a->size) {                                                  \
      a->size *= 2;                                                            \
      a->array = (T *)realloc(a->array, a->size * sizeof(T));                  \
    }                                                                          \
    a->array[a->used++] = element;                                             \
  }                                                                            \
                                                                               \
  void remove_vector_##T(vector_##T *a, T element) {                           \
    int i, j;                                                                  \
    for (i = 0; i < a->used; i++) {                                            \
      if (a->array[i] == element) {                                            \
        for (j = i; j < a->used - 1; j++) {                                    \
          a->array[j] = a->array[j + 1];                                       \
        }                                                                      \
        a->used--;                                                             \
        return;                                                                \
      }                                                                        \
    }                                                                          \
  }                                                                            \
                                                                               \
  void foreach_vector_##T(vector_##T *a, void (*func)(T)) {                    \
    for (int i = 0; i < a->used; i++) {                                        \
      func(a->array[i]);                                                       \
    }                                                                          \
  }                                                                            \
                                                                               \
  void free_vector_##T(vector_##T *a) {                                        \
    free(a->array);                                                            \
    a->array = NULL;                                                           \
    a->used = a->size = 0;                                                     \
  }
#endif
