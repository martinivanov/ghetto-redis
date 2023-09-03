#ifndef VECTOR_H
#define VECTOR_H
#include "logging.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define VECTOR_TYPE(T)                                                         \
  typedef struct {                                                             \
    T *array;                                                                  \
    size_t used;                                                               \
    size_t size;                                                               \
  } vector_##T;                                                                \
                                                                               \
  static inline void init_vector_##T(vector_##T *a, size_t initialSize) {                    \
    a->array = (T *)malloc(initialSize * sizeof(T));                           \
    a->used = 0;                                                               \
    a->size = initialSize;                                                     \
  }                                                                            \
                                                                               \
  static inline size_t capacity_vector_##T(vector_##T *a) { return a->size; }                \
                                                                               \
  static inline size_t size_vector_##T(vector_##T *a) { return a->used; }                    \
                                                                               \
  static inline void resize_vector_##T(vector_##T *a, size_t new_size) {                     \
    T *new_arr = (T *)realloc(a->array, new_size * sizeof(T));                 \
    if (!new_arr) {                                                            \
      panic("couldn't resize");                                                \
    }                                                                          \
    a->array = new_arr;                                                        \
    a->size = new_size;                                                        \
  }                                                                            \
                                                                               \
  static inline void insert_vector_##T(vector_##T *a, T element) {                           \
    if (a->used == a->size) {                                                  \
      a->size *= 2;                                                            \
      a->array = (T *)realloc(a->array, a->size * sizeof(T));                  \
    }                                                                          \
    a->array[a->used++] = element;                                             \
  }                                                                            \
                                                                               \
  static inline void remove_vector_##T(vector_##T *a, T element, bool (*pred)(T, T)) {       \
    size_t i, j;                                                               \
    for (i = 0; i < a->used; i++) {                                            \
      if (pred(a->array[i], element)) {                                        \
        for (j = i; j < a->used - 1; j++) {                                    \
          a->array[j] = a->array[j + 1];                                       \
        }                                                                      \
        a->used--;                                                             \
        return;                                                                \
      }                                                                        \
    }                                                                          \
  }                                                                            \
                                                                               \
  static inline void foreach_vector_##T(vector_##T *a, void (*func)(T)) {                    \
    for (size_t i = 0; i < a->used; i++) {                                     \
      func(a->array[i]);                                                       \
    }                                                                          \
  }                                                                            \
                                                                               \
  static inline void free_vector_##T(vector_##T *a) {                                        \
    free(a->array);                                                            \
    a->array = NULL;                                                           \
    a->used = a->size = 0;                                                     \
  }                                                                            \
                                                                               \
  static inline void clear_vector_##T(vector_##T *a) { a->used = 0; }

#define VECTOR_TYPE_PTR(T)                                                     \
  typedef struct {                                                             \
    T **array;                                                                 \
    size_t used;                                                               \
    size_t size;                                                               \
  } vector_##T##_ptr;                                                          \
                                                                               \
  static inline void init_vector_##T##_ptr(vector_##T##_ptr *a, size_t initialSize) {        \
    a->array = (T **)malloc(initialSize * sizeof(T *));                        \
    a->used = 0;                                                               \
    a->size = initialSize;                                                     \
  }                                                                            \
                                                                               \
  static inline size_t capacity_vector_##T##_ptr(vector_##T##_ptr *a) { return a->size; }    \
                                                                               \
  static inline size_t size_vector_##T##_ptr(vector_##T##_ptr *a) { return a->used; }        \
                                                                               \
  static inline void resize_vector_##T##_ptr(vector_##T##_ptr *a, size_t new_size) {         \
    T **new_arr = (T **)realloc(a->array, new_size * sizeof(T *));             \
    if (!new_arr) {                                                            \
      panic("couldn't resize");                                                \
    }                                                                          \
    a->array = new_arr;                                                        \
    a->size = new_size;                                                        \
  }                                                                            \
                                                                               \
  static inline void insert_vector_##T##_ptr(vector_##T##_ptr *a, T *element) {              \
    if (a->used == a->size) {                                                  \
      a->size *= 2;                                                            \
      a->array = (T **)realloc(a->array, a->size * sizeof(T *));               \
    }                                                                          \
    a->array[a->used++] = element;                                             \
  }                                                                            \
                                                                               \
  static inline void remove_vector_##T##_ptr(vector_##T##_ptr *a, T *element,                \
                               bool (*pred)(T *, T *)) {                       \
    size_t i, j;                                                               \
    for (i = 0; i < a->used; i++) {                                            \
      if (pred(a->array[i], element)) {                                        \
        for (j = i; j < a->used - 1; j++) {                                    \
          a->array[j] = a->array[j + 1];                                       \
        }                                                                      \
        a->used--;                                                             \
        return;                                                                \
      }                                                                        \
    }                                                                          \
  }                                                                            \
                                                                               \
  static inline void foreach_vector_##T##_ptr(vector_##T##_ptr *a, void (*func)(T *)) {      \
    for (size_t i = 0; i < a->used; i++) {                                     \
      func(a->array[i]);                                                       \
    }                                                                          \
  }                                                                            \
                                                                               \
  static inline void free_vector_##T##_ptr(vector_##T##_ptr *a) {                            \
    free(a->array);                                                            \
    a->array = NULL;                                                           \
    a->used = a->size = 0;                                                     \
  }                                                                            \
                                                                               \
  static inline void clear_vector_##T##_ptr(vector_##T##_ptr *a) { a->used = 0; }
#endif
