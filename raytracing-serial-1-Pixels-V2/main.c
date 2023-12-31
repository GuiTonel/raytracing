/**
 * Copyright (c) 2020, Evgeniy Morozov
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdio.h>
#include <pthread.h>
#include "hittables/rt_hittable_list.h"
#include "rt_camera.h"
#include "rt_skybox_simple.h"
#include <errno.h>
#include <string.h>
#include <scenes/rt_scenes.h>
#include <assert.h>

static void show_usage(const char *program_name, int err);

static colour_t ray_colour(const ray_t *ray, const rt_hittable_list_t *list, rt_skybox_t *skybox, int child_rays)
{
    if (child_rays <= 0)
    {
        return colour(0, 0, 0);
    }

    rt_hit_record_t record;
    // MOST EXPESIVE FUNCTION - CALCULATE THE RAY COLISION FOR EACH OBJECT IN THE WORLD
    if (!rt_hittable_list_hit_test(list, ray, 0.001, INFINITY, &record))
    {
        return rt_skybox_value(skybox, ray);
    }

    ray_t scattered;
    colour_t attenuation;
    colour_t emitted = rt_material_emit(record.material, record.u, record.v, &record.p);

    // ONLY SERIAL
    if (rt_material_scatter(record.material, ray, &record, &attenuation, &scattered))
    {
        return vec3_sum(emitted, vec3_multiply(attenuation, ray_colour(&scattered, list, skybox, child_rays - 1)));
    }
    return emitted;
}

pthread_mutex_t mutex;

long G_NUMBER_OF_SAMPLES;
rt_camera_t *G_CAMERA;
rt_hittable_list_t *G_WORLD;
rt_skybox_t *G_SKYBOX;
int G_CHILD_RAYS;
int G_IMAGE_WIDTH;
int G_IMAGE_HEIGHT;
int G_BATCH_SIZE;
int G_BATCH_NUMBER;
volatile int NEXT_BATCH;

colour_t** volatile PIXELS_LIST;

void render(const int IMAGE_WIDTH, const int IMAGE_HEIGHT, long number_of_samples, rt_camera_t *camera, rt_hittable_list_t *world, rt_skybox_t *skybox, const int CHILD_RAYS, FILE *out_file)
{   
    // PARALLELIZABLE 
    for (int j = IMAGE_HEIGHT - 1; j >= 0; --j)
    {
        fprintf(stderr, "\rScanlines remaining: %d", j);
        fflush(stderr);
        for (int i = 0; i < IMAGE_WIDTH; ++i)
        {
            colour_t pixel = colour(0, 0, 0);
            // Antialiasing CODE
            for (int s = 0; s < number_of_samples; ++s)
            {
                double u = (double)(i + rt_random_double(0, 1)) / (IMAGE_WIDTH - 1);
                double v = (double)(j + rt_random_double(0, 1)) / (IMAGE_HEIGHT - 1);

                ray_t ray = rt_camera_get_ray(camera, u, v);
                vec3_add(&pixel, ray_colour(&ray, world, skybox, CHILD_RAYS));
            }
            rt_write_colour(out_file, pixel, number_of_samples);
        }
    }
}

void* render_threads(int width_begin, int width_end, colour_t* pixels)
{   
    int p = 0;
    int h = 0;
    int w = 0;
    for (int i = width_begin; i < width_end; ++i)
    {   
        h = (i/G_IMAGE_WIDTH);
        w = ((i)%G_IMAGE_WIDTH);

        colour_t pixel = colour(0, 0, 0);
        // Antialiasing CODE
        for (int s = 0; s < G_NUMBER_OF_SAMPLES; ++s)
        {
            double u = (double)(w + rt_random_double(0, 1)) / (G_IMAGE_WIDTH - 1);
            double v = (double)(h + rt_random_double(0, 1)) / (G_IMAGE_HEIGHT - 1);

            ray_t ray = rt_camera_get_ray(G_CAMERA, u, v);
            vec3_add(&pixel, ray_colour(&ray, G_WORLD, G_SKYBOX, G_CHILD_RAYS));
        }
        pixels[p] = pixel;
        p++;
    }
}

void* worker(void *arg) {
    int width_begin = 0;
    int width_end = 0;
    int i;

    do {
        pthread_mutex_lock(&mutex);
        i = NEXT_BATCH;
        fprintf(stdout, "\rProcessing Batch: %d of %d.", i+1, G_BATCH_NUMBER);
        fflush(stdout);
        NEXT_BATCH += 1; 
        pthread_mutex_unlock(&mutex);

        width_begin = (G_BATCH_SIZE*i);
        width_end = G_BATCH_SIZE*(i+1);

        if (width_end > G_IMAGE_WIDTH * G_IMAGE_HEIGHT ) {
            width_end = G_IMAGE_WIDTH * G_IMAGE_HEIGHT;
        }

        render_threads(width_begin, width_end, PIXELS_LIST[i]);

    } while (NEXT_BATCH<G_BATCH_NUMBER);

    pthread_exit(NULL);
}

int main(int argc, char const *argv[])
{
    const char *number_of_samples_str = NULL;
    const char *number_of_threads_str = NULL;
    const char *scene_id_str = NULL;
    const char *file_name = NULL;
    bool verbose = false;
    //pthread_exit(NULL);
    // Parse console arguments
    for (int i = 1; i < argc; ++i)
    {
        if (0 == strcmp(argv[i], "-s") || 0 == strcmp(argv[i], "--samples"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Fatal error: Argument '%s' doesn't have a value\n", argv[i]);
                show_usage(argv[0], EXIT_FAILURE);
            }
            number_of_samples_str = argv[++i];
            continue;
        } 
        else if (0 == strcmp(argv[i], "-t") || 0 == strcmp(argv[i], "--threads"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Fatal error: Argument '%s' doesn't have a value\n", argv[i]);
                show_usage(argv[0], EXIT_FAILURE);
            }
            number_of_threads_str = argv[++i];
            continue;
        }
        else if (0 == strcmp(argv[i], "--scene"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Fatal error: Argument '%s' doesn't have a value\n", argv[i]);
                show_usage(argv[0], EXIT_FAILURE);
            }
            scene_id_str = argv[++i];
            continue;
        }
        else if (0 == strcmp(argv[i], "-v") || 0 == strcmp(argv[i], "--verbose"))
        {
            verbose = true;
        }
        else if (0 == strcmp(argv[i], "-h"))
        {
            show_usage(argv[0], EXIT_SUCCESS);
        }
        else if ('-' == *argv[i])
        {
            fprintf(stderr, "Fatal error: Unknown argument '%s'\n", argv[i]);
            show_usage(argv[0], EXIT_FAILURE);
        }
        else if (i + 1 < argc)
        {
            fprintf(stderr, "Fatal error: Too many positional arguments (1 expected)\n");
            show_usage(argv[0], EXIT_FAILURE);
        }
        file_name = argv[i];
    }

    if (verbose)
    {
        fprintf(stderr, "Non-parsed parameters:\n");
        fprintf(stderr, "\t- number of samples: %s\n", number_of_samples_str);
        fprintf(stderr, "\t- scene ID:          %s\n", scene_id_str);
        fprintf(stderr, "\t- file_name:         %s\n", file_name);
    }

    // Parse resulting parameters
    long number_of_samples = 1000;
    if (NULL != number_of_samples_str)
    {
        char *end_ptr = NULL;
        number_of_samples = strtol(number_of_samples_str, &end_ptr, 10);
        if (*end_ptr != '\0')
        {
            fprintf(stderr, "Fatal error: Value of 'samples' is not a correct number\n");
            show_usage(argv[0], EXIT_FAILURE);
        }
    }
    long number_of_threads = 2;
    if (NULL != number_of_threads_str)
    {
        char *end_ptr = NULL;
        number_of_threads = strtol(number_of_threads_str, &end_ptr, 10);
        if (*end_ptr != '\0')
        {
            fprintf(stderr, "Fatal error: Value of 'threads' is not a correct number\n");
            show_usage(argv[0], EXIT_FAILURE);
        }
    }
    rt_scene_id_t scene_id = RT_SCENE_SHOWCASE;
    if (NULL != scene_id_str)
    {
        scene_id = rt_scene_get_id_by_name(scene_id_str);
        if (RT_SCENE_NONE == scene_id)
        {
            fprintf(stderr, "Fatal error: Invalid scene identifier\n");
            show_usage(argv[0], EXIT_FAILURE);
        }
    }

    if (verbose)
    {
        fprintf(stderr, "Parsed parameters:\n");
        fprintf(stderr, "\t- number of samples: %ld\n", number_of_samples);
        fprintf(stderr, "\t- number of threads: %ld\n", number_of_threads);
        fprintf(stderr, "\t- scene ID:          %d\n", scene_id);
        fprintf(stderr, "\t- file_name:         %s\n", file_name);
    }

    // Image parameters
    const double ASPECT_RATIO = 3.0 / 2.0;
    const int IMAGE_WIDTH = 300;
    const int IMAGE_HEIGHT = (int)(IMAGE_WIDTH / ASPECT_RATIO);
    const int CHILD_RAYS = 50;

    // Declare Camera parameters
    point3_t look_from, look_at;
    vec3_t up = point3(0, 1, 0);
    double focus_distance = 10.0, aperture = 0.0, vertical_fov = 40.0;

    // World
    rt_hittable_list_t *world = NULL;
    rt_skybox_t *skybox = NULL;

    // Select a scene from a pre-defined one
    switch (scene_id)
    {
        case RT_SCENE_RANDOM:
            look_from = point3(13, 2, 3);
            look_at = point3(0, 0, 0);
            aperture = 0.1;
            vertical_fov = 20.0;

            skybox = rt_skybox_new_gradient(colour(1, 1, 1), colour(0.5, 0.7, 1));
            world = rt_scene_random();
            break;

        case RT_SCENE_TWO_SPHERES:
            look_from = point3(13, 2, 3);
            look_at = point3(0, 0, 0);
            vertical_fov = 20.0;

            skybox = rt_skybox_new_gradient(colour(1, 1, 1), colour(0.5, 0.7, 1));
            world = rt_scene_two_spheres();
            break;

        case RT_SCENE_TWO_PERLIN_SPHERES:
            look_from = point3(13, 2, 3);
            look_at = point3(0, 0, 0);
            vertical_fov = 20.0;

            skybox = rt_skybox_new_gradient(colour(1, 1, 1), colour(0.5, 0.7, 1));
            world = rt_scene_two_perlin_spheres();
            break;

        case RT_SCENE_EARTH:
            look_from = point3(13, 2, 3);
            look_at = point3(0, 0, 0);
            vertical_fov = 20.0;

            skybox = rt_skybox_new_gradient(colour(1, 1, 1), colour(0.5, 0.7, 1));
            world = rt_scene_earth();
            break;

        case RT_SCENE_LIGHT_SAMPLE:
            look_from = point3(26, 3, 6);
            look_at = point3(0, 2, 0);
            vertical_fov = 20.0;

            skybox = rt_skybox_new_background(colour(0, 0, 0));
            world = rt_scene_light_sample();
            break;

        case RT_SCENE_CORNELL_BOX:
            look_from = point3(278, 278, -800);
            look_at = point3(278, 278, 0);

            skybox = rt_skybox_new_background(colour(0, 0, 0));
            world = rt_scene_cornell_box();
            break;

        case RT_SCENE_INSTANCE_TEST:
            look_from = point3(0, 5, -20);
            look_at = point3(0, 0, 0);
            vertical_fov = 20.0;

            skybox = rt_skybox_new_gradient(colour(1, 1, 1), colour(0.5, 0.7, 1));
            world = rt_scene_instance_test();
            break;

        case RT_SCENE_CORNELL_SMOKE:
            look_from = point3(278, 278, -800);
            look_at = point3(278, 278, 0);

            skybox = rt_skybox_new_background(colour(0, 0, 0));
            world = rt_scene_cornell_box_smoke_boxes();
            break;

        case RT_SCENE_SHOWCASE:
            look_from = point3(478, 278, -600);
            look_at = point3(278, 278, 0);

            skybox = rt_skybox_new_background(colour(0, 0, 0));
            world = rt_scene_showcase();
            break;

        case RT_SCENE_METAL_TEST:
            look_from = point3(0, 5, -10);
            look_at = point3(0, 2, 0);
            vertical_fov = 20.0;

            skybox = rt_skybox_new_gradient(colour(1, 1, 1), colour(0.5, 0.7, 1));
            world = rt_scene_metal_test();
            break;
        case RT_SCENE_NONE:
            fprintf(stderr, "Fatal error: scene id is undefined after parsing the parameters\n");
            return EXIT_FAILURE;
    }

    rt_camera_t *camera =
        rt_camera_new(look_from, look_at, up, vertical_fov, ASPECT_RATIO, aperture, focus_distance, 0.0, 1.0);

    
    float pixels_number = IMAGE_WIDTH*IMAGE_HEIGHT;
    int batch_size = IMAGE_WIDTH;
    int batch_number = pixels_number/batch_size;
    PIXELS_LIST = (colour_t**) malloc(sizeof(colour_t*)*batch_number);

    G_BATCH_SIZE = batch_size;

    pthread_t t[number_of_threads];

    FILE *out_file = stdout;
    if (NULL != file_name)
    {
        out_file = fopen(file_name, "w");
        if (NULL == out_file)
        {
            fprintf(stderr, "Fatal error: Unable to open file %s: %s", file_name, strerror(errno));
            goto cleanup;
        }
    }
    
    G_IMAGE_WIDTH = IMAGE_WIDTH;
    G_IMAGE_HEIGHT = IMAGE_HEIGHT;
    G_NUMBER_OF_SAMPLES = number_of_samples;
    G_CAMERA = camera;
    G_SKYBOX = skybox;
    G_WORLD = world;
    G_CHILD_RAYS = CHILD_RAYS;
    G_BATCH_NUMBER = batch_number;
    for (int i = 0; i<batch_number; i++) {
        PIXELS_LIST[i] = (colour_t*) malloc(sizeof(colour_t)*batch_size);
    }

    NEXT_BATCH = 0;
    for (long i = 0; i < number_of_threads; i++) {
        pthread_create(&t[i], NULL, worker, NULL);
    }

    for (int i = 0; i < number_of_threads; i++){
        pthread_join(t[i], NULL);
    }
    
    fprintf(out_file, "P3\n%d %d\n255\n", IMAGE_WIDTH, IMAGE_HEIGHT);
    for (int i = batch_number - 1; i>=0; i--) {
        for (int j = 0; j<batch_size; j++) {
	        rt_write_colour(out_file, PIXELS_LIST[i][j], number_of_samples);
        }
        free(PIXELS_LIST[i]);
    }
    free(PIXELS_LIST);
    

cleanup:
    // Cleanup
    rt_hittable_list_deinit(world);
    rt_camera_delete(camera);
    rt_skybox_delete(skybox);

    pthread_exit(NULL);

    return EXIT_SUCCESS;
}

static void show_usage(const char *program_name, int err)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "%s [-s|--samples N] [--scene SCENE] [-v|--verbose] [output_file_name]\n", program_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\t-s | --samples      <int>       Number of rays to cast for each pixel\n");
    fprintf(stderr, "\t--scene             <string>    ID of the scene to render. List of available scenes is printed below.\n");
    fprintf(stderr, "\t-v | --verbose                  Enable verbose output\n");
    fprintf(stderr, "\t-h                              Show this message and exit\n");
    fprintf(stderr, "Positional arguments:\n");
    fprintf(stderr, "\toutput_file_name                Name of the output file. Outputs image to console if not specified.\n");
    fprintf(stderr, "Available scenes:\n");
    rt_scene_print_scenes_info(stderr);

    exit(err);
}
