#include "id_generator.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static uint64_t g_salt;
static uint64_t g_counter;
static int g_initialized;

static uint64_t splitmix64(uint64_t value)
{
    uint64_t mixed = value + UINT64_C(0x9E3779B97F4A7C15);

    mixed = (mixed ^ (mixed >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    mixed = (mixed ^ (mixed >> 27)) * UINT64_C(0x94D049BB133111EB);
    return mixed ^ (mixed >> 31);
}

/*
 * Semilla de respaldo si /dev/urandom no esta disponible. No se usa como
 * fuente criptografica: solo evita que dos procesos arrancados a la vez
 * compartan exactamente el mismo estado.
 */
static uint64_t fallback_seed(void)
{
    struct timespec realtime;
    struct timespec monotonic;
    uint64_t value;

    (void)clock_gettime(CLOCK_REALTIME, &realtime);
    (void)clock_gettime(CLOCK_MONOTONIC, &monotonic);

    value = ((uint64_t)realtime.tv_sec << 32) ^ (uint64_t)realtime.tv_nsec;
    value ^= ((uint64_t)monotonic.tv_sec << 16) ^ (uint64_t)monotonic.tv_nsec;
    value ^= (uint64_t)(uint32_t)getpid() << 32;
    value ^= (uint64_t)(uintptr_t)&g_salt;

    return splitmix64(value);
}

static int read_random_salt(uint64_t *salt)
{
    int fd;
    ssize_t count;
    int saved_errno;
    int result = -1;

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    do {
        count = read(fd, salt, sizeof(*salt));
    } while (count < 0 && errno == EINTR);

    saved_errno = errno;
    if (count == (ssize_t)sizeof(*salt)) {
        result = 0;
    } else if (count >= 0) {
        saved_errno = EIO;
    }

    (void)close(fd);
    errno = saved_errno;
    return result;
}

int tl_id_generator_init(void)
{
    struct timespec now;
    uint64_t seed = 0;

    if (g_initialized) {
        errno = EALREADY;
        return -1;
    }

    if (read_random_salt(&seed) < 0) {
        seed = fallback_seed();
    }

    /*
     * La semilla combina la fuente aleatoria (o el respaldo) con el PID y el
     * instante de inicializacion. Dentro de un proceso la unicidad real la
     * garantiza el contador global de tl_id_generator_next(); esta mezcla
     * evita que dos procesos distintos arranquen con el mismo estado.
     */
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        seed ^= (uint64_t)now.tv_sec * UINT64_C(1000000000) +
                (uint64_t)now.tv_nsec;
    }
    seed ^= (uint64_t)(uint32_t)getpid() << 32;
    seed ^= (uint64_t)(uintptr_t)&g_salt;
    seed = splitmix64(seed);

    if (seed == 0) {
        seed = UINT64_C(0xA5A5A5A5DEADBEEF);
    }

    g_salt = seed;
    g_counter = 0;
    g_initialized = 1;
    return 0;
}

void tl_id_generator_reset(void)
{
    g_salt = 0;
    g_counter = 0;
    g_initialized = 0;
}

uint64_t tl_id_generator_next(tl_id_kind_t kind)
{
    uint64_t value;

    if (!g_initialized) {
        errno = EPERM;
        return 0;
    }

    if (kind != TL_ID_KIND_TRACE && kind != TL_ID_KIND_SPAN) {
        errno = EINVAL;
        return 0;
    }

    /*
     * El contador es global para trazas y spans. splitmix64() es una
     * biyeccion, de modo que entradas distintas producen salidas distintas.
     * Al usar un contador que solo avanza, los identificadores generados en
     * este proceso no se repiten ni entre si ni entre trazas y spans.
     *
     * Si splitmix64() produjera 0 (reservado para "sin identificar"), se
     * descarta ese valor y se avanza al siguiente contador. Como la funcion
     * es inyectiva, descartar el 0 no puede introducir una repeticion.
     */
    do {
        if (g_counter == UINT64_MAX) {
            errno = EOVERFLOW;
            return 0;
        }
        g_counter++;
        value = splitmix64(g_salt ^ g_counter);
    } while (value == 0);

    return value;
}
