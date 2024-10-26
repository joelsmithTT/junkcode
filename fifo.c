#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
} CircularBuffer;

bool is_full(const CircularBuffer *cb) { return cb->count == cb->capacity; }
bool is_empty(const CircularBuffer *cb) { return cb->count == 0; }

bool buf_write(CircularBuffer *cb, uint8_t item) {
    if (!cb || is_full(cb)) return false;

    cb->data[cb->head] = item;
    cb->head++;
    cb->head %= cb->capacity;
    cb->count++;

    return true;
}

bool buf_read(CircularBuffer *cb, uint8_t *item) {
    if (!cb || !item || is_empty(cb)) return false;

    *item = cb->data[cb->tail];

    cb->tail++;
    cb->tail %= cb->capacity;
    cb->count--;

    return true;
}

CircularBuffer* buf_create(size_t capacity) {
    if (capacity == 0) return NULL;

    CircularBuffer *cb = malloc(sizeof(CircularBuffer));
    if (!cb) return NULL;

    cb->data = malloc(capacity * sizeof(uint8_t));
    if (!cb->data) {
        free(cb);
        return NULL;
    }

    cb->capacity = capacity;
    cb->head = 0;
    cb->tail = 0;
    cb->count = 0;

    return cb;
}

void buf_destroy(CircularBuffer *cb) {
    if (cb) {
        free(cb->data);
        free(cb);
    }
}

int main() {
    CircularBuffer *cb = buf_create(5);
    uint8_t data;

    assert(is_empty(cb));
    assert(!is_full(cb));

    assert(buf_write(cb, 1));
    assert(buf_write(cb, 2));
    assert(buf_write(cb, 3));
    assert(!is_empty(cb));
    assert(!is_full(cb));
    assert(buf_write(cb, 4));
    assert(buf_write(cb, 5));
    assert(is_full(cb));

    assert(buf_read(cb, &data) && data == 1);
    assert(buf_read(cb, &data) && data == 2);

    assert(buf_write(cb, 6));
    assert(buf_write(cb, 7));

    assert(!buf_write(cb, 8));  // Buffer full
    assert(buf_read(cb, &data) && data == 3);
    assert(buf_read(cb, &data) && data == 4);
    assert(buf_read(cb, &data) && data == 5);
    assert(buf_read(cb, &data) && data == 6);
    assert(buf_read(cb, &data) && data == 7);
    assert(!buf_read(cb, &data));  // Buffer empty

    return 0;
}
