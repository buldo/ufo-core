#include <gmodule.h>
#include <CL/cl.h>

#include "ufo-resource-manager.h"
#include "ufo-filter-cl.h"
#include "ufo-filter.h"
#include "ufo-buffer.h"

struct _UfoFilterClPrivate {
    cl_kernel kernel;
    gchar *file_name;
    gchar *kernel_name;
    gboolean inplace;
    gboolean interframe;
};

GType ufo_filter_cl_get_type(void) G_GNUC_CONST;

G_DEFINE_TYPE(UfoFilterCl, ufo_filter_cl, UFO_TYPE_FILTER);

#define UFO_FILTER_CL_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), UFO_TYPE_FILTER_CL, UfoFilterClPrivate))

enum {
    PROP_0,
    PROP_FILE_NAME,
    PROP_KERNEL,
    PROP_INPLACE,
    PROP_INTERFRAME,
    N_PROPERTIES
};

static GParamSpec *cl_properties[N_PROPERTIES] = { NULL, };

static void activated(EthosPlugin *plugin)
{
}

static void deactivated(EthosPlugin *plugin)
{
}


/* 
 * virtual methods 
 */
static void ufo_filter_cl_initialize(UfoFilter *filter)
{
    /* Here you can code, that is called for each newly instantiated filter */
    /*UfoFilterCl *self = UFO_FILTER_CL(filter);*/
}

static void process_regular(UfoFilter *self,
        UfoFilterClPrivate *priv, 
        cl_command_queue command_queue, 
        cl_kernel kernel)
{
    UfoChannel *input_channel = ufo_filter_get_input_channel(self);
    UfoChannel *output_channel = ufo_filter_get_output_channel(self);
    UfoResourceManager *manager = ufo_resource_manager();

    size_t global_work_size[2];

    UfoBuffer *frame = ufo_channel_pop(input_channel);

    cl_event event;
    gint32 dimensions[4] = { 1, 1, 1, 1 };

    while (frame != NULL) { 
        ufo_buffer_get_dimensions(frame, dimensions);
        global_work_size[0] = (size_t) dimensions[0];
        global_work_size[1] = (size_t) dimensions[1];

        UfoBuffer *result = ufo_resource_manager_request_buffer(manager, UFO_BUFFER_2D, dimensions, NULL, TRUE);
        cl_mem frame_mem = (cl_mem) ufo_buffer_get_gpu_data(frame, command_queue);
        cl_mem result_mem = (cl_mem) ufo_buffer_get_gpu_data(result, command_queue);

        CHECK_ERROR(clSetKernelArg(kernel, 0, sizeof(cl_mem), (void *) &frame_mem));
        CHECK_ERROR(clSetKernelArg(kernel, 1, sizeof(cl_mem), (void *) &result_mem));
        CHECK_ERROR(clSetKernelArg(kernel, 2, sizeof(float)*16*16, NULL));

        /* XXX: For AMD CPU, a clFinish must be issued before enqueuing the
         * kernel. This should be moved to a ufo_kernel_launch method. */
        CHECK_ERROR(clEnqueueNDRangeKernel(command_queue,
            kernel,
            2, NULL, global_work_size, NULL,
            0, NULL, &event));

        CHECK_ERROR(clFinish(command_queue));

        ufo_resource_manager_release_buffer(manager, frame);
        ufo_channel_push(output_channel, result);
        frame = ufo_channel_pop(input_channel);
    }
    ufo_channel_finish(output_channel);
}

static void process_inplace(UfoFilter *self,
        UfoFilterClPrivate *priv, 
        cl_command_queue command_queue, 
        cl_kernel kernel)
{
    UfoChannel *input_channel = ufo_filter_get_input_channel(self);
    UfoChannel *output_channel = ufo_filter_get_output_channel(self);

    size_t local_work_size[2] = { 16, 16 };
    size_t global_work_size[2];
    gint32 dimensions[4];

    UfoBuffer *frame = ufo_channel_pop(input_channel);

    while (frame != NULL) {
        ufo_buffer_get_dimensions(frame, dimensions);
        global_work_size[0] = (size_t) dimensions[0];
        global_work_size[1] = (size_t) dimensions[1];

        cl_mem frame_mem = (cl_mem) ufo_buffer_get_gpu_data(frame, command_queue);

        CHECK_ERROR(clSetKernelArg(kernel, 0, sizeof(cl_mem), (void *) &frame_mem));
        CHECK_ERROR(clSetKernelArg(kernel, 1, sizeof(float)*local_work_size[0]*local_work_size[1], NULL));

        CHECK_ERROR(clEnqueueNDRangeKernel(command_queue,
            kernel,
            2, NULL, global_work_size, NULL,
            0, NULL, NULL));

        CHECK_ERROR(clFinish(command_queue));

        ufo_channel_push(output_channel, frame);
        frame = ufo_channel_pop(input_channel);
    }
    ufo_channel_finish(output_channel);
}

static void process_two_frames(UfoFilter *self,
        UfoFilterClPrivate *priv, 
        cl_command_queue command_queue, 
        cl_kernel kernel)
{
    UfoChannel *input_channel = ufo_filter_get_input_channel(self);
    UfoChannel *output_channel = ufo_filter_get_output_channel(self);
    UfoResourceManager *manager = ufo_resource_manager();

    size_t local_work_size[2] = { 16, 16 };
    size_t global_work_size[2];
    gint32 dimensions[4];

    UfoBuffer *frame1 = ufo_channel_pop(input_channel);
    /* This might block if we receive just one buffer... */
    UfoBuffer *frame2 = ufo_channel_pop(input_channel);

    cl_event event;

    while ((frame1 != NULL) && (frame2 != NULL)) {
        ufo_buffer_get_dimensions(frame1, dimensions);
        global_work_size[0] = (size_t) dimensions[0];
        global_work_size[1] = (size_t) dimensions[1];

        UfoBuffer *result = ufo_resource_manager_request_buffer(manager, UFO_BUFFER_2D, dimensions, NULL, TRUE);
        cl_mem frame1_mem = (cl_mem) ufo_buffer_get_gpu_data(frame1, command_queue);
        cl_mem frame2_mem = (cl_mem) ufo_buffer_get_gpu_data(frame2, command_queue);
        cl_mem result_mem = (cl_mem) ufo_buffer_get_gpu_data(result, command_queue);

        CHECK_ERROR(clSetKernelArg(kernel, 0, sizeof(cl_mem), (void *) &frame1_mem));
        CHECK_ERROR(clSetKernelArg(kernel, 1, sizeof(cl_mem), (void *) &frame2_mem));
        CHECK_ERROR(clSetKernelArg(kernel, 2, sizeof(cl_mem), (void *) &result_mem));
        CHECK_ERROR(clSetKernelArg(kernel, 3, sizeof(float)*local_work_size[0]*local_work_size[1], NULL));

        /* XXX: For AMD CPU, a clFinish must be issued before enqueuing the
         * kernel. This should be moved to a ufo_kernel_launch method. */
        CHECK_ERROR(clEnqueueNDRangeKernel(command_queue,
            kernel,
            2, NULL, global_work_size, local_work_size,
            0, NULL, &event));

        CHECK_ERROR(clFinish(command_queue));

        ufo_resource_manager_release_buffer(manager, frame1);
        frame1 = frame2;
        frame2 = ufo_channel_pop(input_channel);
        ufo_channel_push(output_channel, result);
    }
    ufo_channel_finish(output_channel);
}

/*
 * This is the main method in which the filter processes one buffer after
 * another.
 */
static void ufo_filter_cl_process(UfoFilter *filter)
{
    g_return_if_fail(UFO_IS_FILTER(filter));
    UfoFilterClPrivate *priv = UFO_FILTER_CL_GET_PRIVATE(filter);
    UfoChannel *output_channel = ufo_filter_get_output_channel(filter);

    cl_command_queue command_queue = (cl_command_queue) ufo_filter_get_command_queue(filter);

    UfoResourceManager *manager = ufo_resource_manager();
    GError *error = NULL;

    /* TODO: right now it's not possible to have the kernel be loaded upfront... */
    ufo_resource_manager_add_program(manager, priv->file_name, NULL, &error);

    if (error != NULL) {
        g_warning("%s", error->message);
        g_error_free(error);
        return;
    }

    cl_kernel kernel = ufo_resource_manager_get_kernel(manager, priv->kernel_name, &error);
    if (error != NULL) {
        g_warning("%s", error->message);
        g_error_free(error);
    }
    if (!kernel) {
        ufo_channel_finish(output_channel);
        return;
    }

    if (priv->interframe)
        process_two_frames(filter, priv, command_queue, kernel);
    else if (priv->inplace)
        process_inplace(filter, priv, command_queue, kernel);
    else
        process_regular(filter, priv, command_queue, kernel);
    
    clReleaseKernel(kernel);
}

static void ufo_filter_cl_set_property(GObject *object,
    guint           property_id,
    const GValue    *value,
    GParamSpec      *pspec)
{
    UfoFilterCl *self = UFO_FILTER_CL(object);

    /* Handle all properties accordingly */
    switch (property_id) {
        case PROP_FILE_NAME:
            g_free(self->priv->file_name);
            self->priv->file_name = g_strdup(g_value_get_string(value));
            break;
        case PROP_KERNEL:
            g_free(self->priv->kernel_name);
            self->priv->kernel_name = g_strdup(g_value_get_string(value));
            break;
        case PROP_INPLACE:
            self->priv->inplace = g_value_get_boolean(value);
            break;
        case PROP_INTERFRAME:
            self->priv->interframe = g_value_get_boolean(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void ufo_filter_cl_get_property(GObject *object,
    guint       property_id,
    GValue      *value,
    GParamSpec  *pspec)
{
    UfoFilterCl *self = UFO_FILTER_CL(object);

    switch (property_id) {
        case PROP_FILE_NAME:
            g_value_set_string(value, self->priv->file_name);
            break;
        case PROP_KERNEL:
            g_value_set_string(value, self->priv->kernel_name);
            break;
        case PROP_INPLACE:
            g_value_set_boolean(value, self->priv->inplace);
            break;
        case PROP_INTERFRAME:
            g_value_set_boolean(value, self->priv->interframe);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void ufo_filter_cl_class_init(UfoFilterClClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    EthosPluginClass *plugin_class = ETHOS_PLUGIN_CLASS(klass);
    UfoFilterClass *filter_class = UFO_FILTER_CLASS(klass);

    gobject_class->set_property = ufo_filter_cl_set_property;
    gobject_class->get_property = ufo_filter_cl_get_property;
    plugin_class->activated = activated;
    plugin_class->deactivated = deactivated;
    filter_class->initialize = ufo_filter_cl_initialize;
    filter_class->process = ufo_filter_cl_process;

    cl_properties[PROP_FILE_NAME] = 
        g_param_spec_string("file",
            "File in which the kernel resides",
            "File in which the kernel resides",
            "",
            G_PARAM_READWRITE);

    cl_properties[PROP_KERNEL] = 
        g_param_spec_string("kernel",
            "Kernel name",
            "Kernel name",
            "",
            G_PARAM_READWRITE);

    cl_properties[PROP_INPLACE] = 
        g_param_spec_boolean("inplace",
            "Expect output buffer or calculate inplace",
            "Expect output buffer or calculate inplace",
            TRUE,
            G_PARAM_READWRITE);

    cl_properties[PROP_INTERFRAME] = 
        g_param_spec_boolean("interframe",
            "Use two frames as an input for a function",
            "Use two frames as an input for a function",
            FALSE,
            G_PARAM_READWRITE);

    g_object_class_install_property(gobject_class, PROP_FILE_NAME, cl_properties[PROP_FILE_NAME]);
    g_object_class_install_property(gobject_class, PROP_KERNEL, cl_properties[PROP_KERNEL]);
    g_object_class_install_property(gobject_class, PROP_INPLACE, cl_properties[PROP_INPLACE]);
    g_object_class_install_property(gobject_class, PROP_INTERFRAME, cl_properties[PROP_INTERFRAME]);

    g_type_class_add_private(gobject_class, sizeof(UfoFilterClPrivate));
}

static void ufo_filter_cl_init(UfoFilterCl *self)
{
    UfoFilterClPrivate *priv = self->priv = UFO_FILTER_CL_GET_PRIVATE(self);
    priv->file_name = NULL;
    priv->kernel_name = NULL;
    priv->kernel = NULL;
    priv->inplace = TRUE;
}

G_MODULE_EXPORT EthosPlugin *ethos_plugin_register(void)
{
    return g_object_new(UFO_TYPE_FILTER_CL, NULL);
}
