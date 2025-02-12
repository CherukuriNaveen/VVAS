/*
 * Copyright 2020 Xilinx, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * file: ivas_xdpuinfer.cpp
 *
 * ivas_xdpuinfer is the dynamic library used with gstreamer ivas filter
 * plugins to have a generic interface for Xilinx DPU Library and applications.
 * ivas_xdpuinfer supports different models of DPU based on model class.
 * Any new class can be added with minimal effort.
 *
 * Example json file parameters required for ivas_xdpuinfer
 * {
 * "xclbin-location":"/usr/lib/dpu.xclbin",
 * "ivas-library-repo": "/usr/local/lib/ivas/",
 * "element-mode":"inplace",
 * "kernels" :[
 *  {
 *    "library-name":"libivas_xdpuinfer.so",
 *    "config": {
 *      "model-name" : "resnet50",
 *      "model-class" : "CLASSIFICATION",
 *      "model-path" : "/usr/share/vitis_ai_library/models/",
 *      "run_time_model" : flase,
 *      "need_preprocess" : true,
 *      "performance_test" : true,
 *      "debug_level" : 1
 *    }
 *   }
 *  ]
 * }
 *
 * Details of above parametres id under "struct ivas_xkpriv"
 *
 * Example pipe:
 * gst-launch-1.0 filesrc location="./images/001.bgr" blocksize=150528 num-buffers=1 !  \
 * videoparse width=224 height=224 framerate=30/1 format=16 ! \
 * ivas_xfilter name="kernel1" kernels-config="./json_files/kernel_resnet50.json" ! \
 * ivas_xfilter name="kernel2" kernels-config="./json_files/kernel_testresnet50.json" ! \
 * filesink location=./resnet_output_224_224.bgr
 */

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <fstream>

#include <vitis/ai/bounded_queue.hpp>
#include <vitis/ai/env_config.hpp>

extern "C"
{
#include <ivas/ivas_kernel.h>
}
#include <gst/ivas/gstinferencemeta.h>
#include <gst/ivas/gstivasinpinfer.h>

#include "ivas_xdpupriv.hpp"
#include "ivas_xdpumodels.hpp"

#ifdef ENABLE_CLASSIFICATION
#include "ivas_xclassification.hpp"
#endif
#ifdef ENABLE_YOLOV3
#include "ivas_xyolov3.hpp"
#endif
#ifdef ENABLE_FACEDETECT
#include "ivas_xfacedetect.hpp"
#endif
#ifdef ENABLE_REID
#include "ivas_xreid.hpp"
#endif
#ifdef ENABLE_SSD
#include "ivas_xssd.hpp"
#endif
#ifdef ENABLE_REFINEDET
#include "ivas_xrefinedet.hpp"
#endif
#ifdef ENABLE_TFSSD
#include "ivas_xtfssd.hpp"
#endif
#ifdef ENABLE_YOLOV2
#include "ivas_xyolov2.hpp"
#endif

using namespace cv;
using namespace std;

ivas_xdpumodel::~ivas_xdpumodel ()
{
}

/**
 * fileexists () - Check either file exists or not
 *
 * check either able to open the file whoes path is in name
 *
 */
inline bool
fileexists (const string & name)
{
  struct stat buffer;
  return (stat (name.c_str (), &buffer) == 0);
}

/**
 * modelexits () - Validate model paths and model files names
 *
 */
static
    std::string
modelexits (ivas_xkpriv * kpriv)
{
  auto elf_name =
      kpriv->modelpath + "/" + kpriv->modelname + "/" + kpriv->modelname +
      ".elf";
  auto xmodel_name =
      kpriv->modelpath + "/" + kpriv->modelname + "/" + kpriv->modelname +
      ".xmodel";
  auto prototxt_name =
      kpriv->modelpath + "/" + kpriv->modelname + "/" + kpriv->modelname +
      ".prototxt";

  if (!fileexists (prototxt_name)) {
    LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level, "%s not found",
        prototxt_name.c_str ());
    elf_name = "";
    return elf_name;
  }

  if (fileexists (xmodel_name))
    return xmodel_name;
  else if (fileexists (elf_name))
    return elf_name;
  else {
    LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
        "xmodel or elf file not found");
    LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level, "%s", elf_name.c_str ());
    LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level, "%s", xmodel_name.c_str ());
    elf_name = "";
  }

  return elf_name;
}

/**
 * readlabel () - Read label from json file
 *
 * Read labels and construct the label array
 * used by class files to fill meta data for label.
 *
 */
labels *
readlabel (ivas_xkpriv * kpriv, char *json_file)
{
  json_t *root = NULL, *karray, *label, *value;
  json_error_t error;
  unsigned int num_labels;
  labels *labelptr;

  /* get root json object */
  root = json_load_file (json_file, JSON_DECODE_ANY, &error);
  if (!root) {
    LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
        "failed to load json file(%s) reason %s", json_file, error.text);
    return NULL;
  }

  value = json_object_get (root, "model-name");
  if (json_is_string (value)) {
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "label is for model %s",
        (char *) json_string_value (value));
  }


  value = json_object_get (root, "num-labels");
  if (!value || !json_is_integer (value)) {
    LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
        "num-labels not found in %s", json_file);
    return NULL;
  } else {
    num_labels = json_integer_value (value);
    labelptr = (labels *) calloc (num_labels, sizeof (labels));
    kpriv->max_labels = num_labels;
  }

  /* get kernels array */
  karray = json_object_get (root, "labels");
  if (!karray) {
    LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
        "failed to find key labels");
    goto error;
  }

  if (!json_is_array (karray)) {
    LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
        "labels key is not of array type");
    goto error;
  }


  if (num_labels != json_array_size (karray)) {
    LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
        "number of labels(%u) != karray size(%lu)\n", num_labels,
        json_array_size (karray));
    goto error;
  }

  for (unsigned int index = 0; index < num_labels; index++) {
    label = json_array_get (karray, index);
    if (!label) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "failed to get label object");
      goto error;
    }
    value = json_object_get (label, "label");
    if (!value || !json_is_integer (value)) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "label num found for array %d", index);
      goto error;
    }

    /*label is index of array */
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "label %d",
        (int) json_integer_value (value));
    labels *lptr = labelptr + (int) json_integer_value (value);
    lptr->label = (int) json_integer_value (value);

    value = json_object_get (label, "name");
    if (!json_is_string (value)) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "name is not found for array %d", index);
      goto error;
    } else {
      lptr->name = (char *) json_string_value (value);
      LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "name %s",
          lptr->name.c_str ());
    }
    value = json_object_get (label, "display_name");
    if (!json_is_string (value)) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "display name is not found for array %d", index);
      goto error;
    } else {
      lptr->display_name = (char *) json_string_value (value);
      LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "display_name %s",
          lptr->display_name.c_str ());
    }

  }
  return labelptr;
error:
  free (labelptr);
  return NULL;
}

int
ivas_xclass_to_num (char *name)
{
  int nameslen = 0;
  while (ivas_xmodelclass[nameslen] != NULL) {
    if (!strcmp (ivas_xmodelclass[nameslen], name))
      return nameslen;
    nameslen++;
  }
  return IVAS_XCLASS_NOTFOUND;
}

IVASVideoFormat
ivas_fmt_to_xfmt (char *name)
{
  if (!strncmp (name, "RGB", 3))
    return IVAS_VFMT_RGB8;
  else if (!strncmp (name, "BGR", 3))
    return IVAS_VFMT_BGR8;
  else
    return IVAS_VMFT_UNKNOWN;
}


long long
get_time ()
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return ((long long) tv.tv_sec * 1000000 + tv.tv_usec) +
      42 * 60 * 60 * INT64_C (1000000);
}

/**
 * ivas_xsetcaps() - Create and set capability of the DPU
 *
 * DPU works in pass through mode so only Sink pads are created by function.
 * The model supported width and height is at cap[0],
 * which means ivas_xdpuinfer first preference for negotiation.
 * Then next caps will support range 1 to 1024  and BGR and RGB,
 * which means upstream plugin can work within this range and
 * DPU library will do scaling.
 */

int
ivas_xsetcaps (ivas_xkpriv * kpriv, ivas_xdpumodel * model)
{
  kernelcaps *new_caps;
  IVASKernel *handle = kpriv->handle;

  ivas_caps_set_pad_nature (handle, IVAS_PAD_RIGID);

  new_caps =
      ivas_caps_new (false, model->requiredheight (), 0, false,
      model->requiredwidth (), 0, kpriv->modelfmt, 0);
  if (!new_caps)
    return false;
  if (ivas_caps_add_to_sink (handle, new_caps, 0) == false) {
    ivas_caps_free (handle);
    return false;
  }
  new_caps =
      ivas_caps_new (true, 1, 1024, true, 1, 1920, IVAS_VFMT_BGR8,
      IVAS_VFMT_RGB8, 0);
  if (!new_caps)
    return false;
  if (ivas_caps_add_to_sink (handle, new_caps, 0) == false) {
    ivas_caps_free (handle);
    return false;
  }

  if (kpriv->log_level == LOG_LEVEL_DEBUG)
    ivas_caps_print (handle);

  return true;
}

#if 0
int
ivas_xsetcaps (ivas_xkpriv * kpriv, ivas_xdpumodel * model)
{
  IVASKernel *handle = kpriv->handle;

  ivaspads *padinfo = (ivaspads *) calloc (1, sizeof (ivaspads));

  padinfo->nature = IVAS_PAD_RIGID;
  //padinfo->nature = IVAS_PAD_FLEXIBLE;
  padinfo->nu_sinkpad = 1;
  padinfo->nu_srcpad = 1;

  kernelpads **sinkpads = (kernelpads **) calloc (1, sizeof (kernelpads *));
  /* Create memory of all sink pad */
  for (int i = 0; i < padinfo->nu_sinkpad; i++) {
    sinkpads[i] = (kernelpads *) calloc (1, sizeof (kernelpads));
    sinkpads[i]->nu_caps = 2;
    sinkpads[i]->kcaps = (kernelcaps **) calloc (sinkpads[i]->nu_caps,
        sizeof (kernelcaps *));
    /* Create memory for all caps */
    for (int j = 0; j < sinkpads[i]->nu_caps; j++) {
      sinkpads[i]->kcaps[j] = (kernelcaps *) calloc (1, sizeof (kernelcaps));
    }                           //sinkpad[i]->nu_caps

    /*Fill all caps */
    sinkpads[i]->kcaps[0]->range_height = false;
    sinkpads[i]->kcaps[0]->lower_height = model->requiredheight ();
    sinkpads[i]->kcaps[0]->lower_width = model->requiredwidth ();
    sinkpads[i]->kcaps[0]->num_fmt = 1;
    sinkpads[i]->kcaps[0]->fmt =
        (IVASVideoFormat *) calloc (sinkpads[i]->kcaps[0]->num_fmt,
        sizeof (IVASVideoFormat));
    sinkpads[i]->kcaps[0]->fmt[0] = IVAS_VFMT_BGR8;

    sinkpads[i]->kcaps[1]->range_height = true;
    sinkpads[i]->kcaps[1]->lower_height = 1;
    sinkpads[i]->kcaps[1]->upper_height = 1024;

    sinkpads[i]->kcaps[1]->range_width = true;
    sinkpads[i]->kcaps[1]->lower_width = 1;
    sinkpads[i]->kcaps[1]->upper_width = 1920;
    sinkpads[i]->kcaps[1]->num_fmt = 2;

    sinkpads[i]->kcaps[1]->fmt =
        (IVASVideoFormat *) calloc (sinkpads[i]->kcaps[1]->num_fmt,
        sizeof (IVASVideoFormat));
    sinkpads[i]->kcaps[1]->fmt[0] = IVAS_VFMT_BGR8;
    sinkpads[i]->kcaps[1]->fmt[1] = IVAS_VFMT_RGB8;

#if 0
    /* Just for referance */
    sinkpads[i]->kcaps[2]->range_height = true;
    sinkpads[i]->kcaps[2]->lower_height = 1;
    sinkpads[i]->kcaps[2]->upper_height = 1024;
    sinkpads[i]->kcaps[2]->lower_width = 1;
    sinkpads[i]->kcaps[2]->upper_width = 1920;
    sinkpads[i]->kcaps[2]->fmt = IVAS_VFMT_RGB8;
#endif
  }                             //padinfo->nu_sinkpad

  padinfo->sinkpads = sinkpads;
  handle->padinfo = padinfo;

  return true;
}
#endif
/**
 * ivas_xinitmodel() - Initialize the required models
 *
 * This function calls the constructor of the CLASS provided in the json file
 * and calls create () of the dpu library of respective model.
 * Along with that it check the return from constructor either
 * label file is needed or not.
 */
ivas_xdpumodel *
ivas_xinitmodel (ivas_xkpriv * kpriv, int modelclass)
{
  LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "enter");
  ivas_xdpumodel *model = NULL;
  kpriv->labelptr = NULL;
  kpriv->labelflags = IVAS_XLABEL_NOT_REQUIRED;

  LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "Creating model %s",
      kpriv->modelname.c_str ());

  const auto labelfile =
      kpriv->modelpath + "/" + kpriv->modelname + "/" + "label.json";
  if (fileexists (labelfile)) {
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level,
        "Label file %s found\n", labelfile.c_str ());
    kpriv->labelptr = readlabel (kpriv, (char *) labelfile.c_str ());
  }

  switch (modelclass) {
#ifdef ENABLE_CLASSIFICATION
    case IVAS_XCLASS_CLASSIFICATION:
    {
      model =
          new ivas_xclassification (kpriv, kpriv->elfname,
          kpriv->need_preprocess);
      break;
    }
#endif
#ifdef ENABLE_YOLOV3
    case IVAS_XCLASS_YOLOV3:
    {
      model = new ivas_xyolov3 (kpriv, kpriv->elfname, kpriv->need_preprocess);
      break;
    }
#endif
#ifdef ENABLE_FACEDETECT
    case IVAS_XCLASS_FACEDETECT:
    {
      model =
          new ivas_xfacedetect (kpriv, kpriv->elfname, kpriv->need_preprocess);
      break;
    }
#endif
#ifdef ENABLE_REID
    case IVAS_XCLASS_REID:
    {
      model = new ivas_xreid (kpriv, kpriv->elfname, kpriv->need_preprocess);
      break;
    }
#endif
#ifdef ENABLE_SSD
    case IVAS_XCLASS_SSD:
    {
      model = new ivas_xssd (kpriv, kpriv->elfname, kpriv->need_preprocess);
      break;
    }
#endif
#ifdef ENABLE_REFINEDET
    case IVAS_XCLASS_REFINEDET:
    {
      model =
          new ivas_xrefinedet (kpriv, kpriv->elfname, kpriv->need_preprocess);
      break;
    }
#endif
#ifdef ENABLE_TFSSD
    case IVAS_XCLASS_TFSSD:
    {
      model = new ivas_xtfssd (kpriv, kpriv->elfname, kpriv->need_preprocess);
      break;
    }
#endif
#ifdef ENABLE_YOLOV2
    case IVAS_XCLASS_YOLOV2:
    {
      model = new ivas_xyolov2 (kpriv, kpriv->elfname, kpriv->need_preprocess);
      break;
    }
#endif

    default:
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level, "Not supported model");
      free (kpriv);
      return NULL;
  }

  if ((kpriv->labelflags & IVAS_XLABEL_REQUIRED)
      && (kpriv->labelflags & IVAS_XLABEL_NOT_FOUND)) {
    kpriv->model->close ();
    delete kpriv->model;
    kpriv->model = NULL;
    kpriv->modelclass = IVAS_XCLASS_NOTFOUND;

    if (kpriv->labelptr != NULL)
      free (kpriv->labelptr);

    return NULL;
  }

  ivas_xsetcaps (kpriv, model);

  return model;
}

/**
 * ivas_xrunmodel() - Run respective model
 */
int
ivas_xrunmodel (ivas_xkpriv * kpriv, cv::Mat & image,
    GstInferenceMeta * infer_meta, IVASFrame * inframe)
{
  LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "enter");
  ivas_xdpumodel *model = (ivas_xdpumodel *) kpriv->model;

  if (model->run (kpriv, image, infer_meta) != true) {
    LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level, "Model run failed %s",
        kpriv->modelname.c_str ());
    return -1;
  }
  return true;
}

extern "C"
{

  int32_t xlnx_kernel_init (IVASKernel * handle)
  {
    ivas_xkpriv *kpriv = (ivas_xkpriv *) calloc (1, sizeof (ivas_xkpriv));
      kpriv->handle = handle;

    json_t *jconfig = handle->kernel_config;
    json_t *val;                /* kernel config from app */

    /* parse config */

      val = json_object_get (jconfig, "debug_level");
    if (!val || !json_is_integer (val))
        kpriv->log_level = LOG_LEVEL_WARNING;
    else
        kpriv->log_level = json_integer_value (val);
      LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "enter");

      val = json_object_get (jconfig, "run_time_model");
    if (!val || !json_is_boolean (val))
        kpriv->run_time_model = 0;
    else
        kpriv->run_time_model = json_boolean_value (val);

      val = json_object_get (jconfig, "performance_test");
    if (!val || !json_is_boolean (val))
        kpriv->performance_test = false;
    else
        kpriv->performance_test = json_boolean_value (val);

      val = json_object_get (jconfig, "need_preprocess");
    if (!val || !json_is_boolean (val))
        kpriv->need_preprocess = true;
    else
    {
      kpriv->need_preprocess = json_boolean_value (val);
    }

    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level,
        "debug_level = %d, performance_test = %d", kpriv->log_level,
        kpriv->performance_test);

    val = json_object_get (jconfig, "model-format");
    if (!json_is_string (val)) {
      LOG_MESSAGE (LOG_LEVEL_WARNING, kpriv->log_level,
          "model-format is not proper, taking BGR as default\n");
      kpriv->modelfmt = IVAS_VFMT_BGR8;
    } else {
      kpriv->modelfmt = ivas_fmt_to_xfmt ((char *) json_string_value (val));
      LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level,
          "model-format %s", (char *) json_string_value (val));
    }
    if (kpriv->modelfmt == IVAS_VMFT_UNKNOWN) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "SORRY NOT SUPPORTED MODEL FORMAT %s",
          (char *) json_string_value (val));
      goto err;
    }
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level,
        "modelfmt = %d, need_preprocess = %d", kpriv->modelfmt,
        kpriv->need_preprocess);

    val = json_object_get (jconfig, "model-path");
    if (!json_is_string (val)) {
      LOG_MESSAGE (LOG_LEVEL_WARNING, kpriv->log_level,
          "model-path is not proper");
      kpriv->modelpath = (char *) "/usr/share/vitis_ai_library/models/";
      LOG_MESSAGE (LOG_LEVEL_WARNING, kpriv->log_level,
          "using default path : %s", kpriv->modelpath.c_str ());
    } else {
      kpriv->modelpath = (char *) json_string_value (val);
    }
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "model-path (%s)",
        kpriv->modelpath.c_str ());
    if (!fileexists (kpriv->modelpath)) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "model-path (%s) not exist", kpriv->modelpath.c_str ());
      goto err;
    }

    if (kpriv->run_time_model) {
      LOG_MESSAGE (LOG_LEVEL_INFO, kpriv->log_level,
          "runtime model load is set");
      handle->kernel_priv = (void *) kpriv;
      return true;
    }

    val = json_object_get (jconfig, "model-class");
    if (!json_is_string (val)) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "model-class is not proper\n");
      goto err;
    }
    kpriv->modelclass = ivas_xclass_to_num ((char *) json_string_value (val));
    if (kpriv->modelclass == IVAS_XCLASS_NOTFOUND) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "SORRY NOT SUPPORTED MODEL CLASS %s",
          (char *) json_string_value (val));
      goto err;
    }

    val = json_object_get (jconfig, "model-name");
    if (!json_is_string (val)) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "model-name is not proper\n");
      goto err;
    }
    kpriv->modelname = (char *) json_string_value (val);

    kpriv->elfname = modelexits (kpriv);
    if (kpriv->elfname.empty ()) {
      goto err;
    }

    LOG_MESSAGE (LOG_LEVEL_INFO, kpriv->log_level, "model-name = %s\n",
        (char *) json_string_value (val));
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "model class is %d",
        kpriv->modelclass);
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "elf class is %s",
        kpriv->elfname.c_str ());

    kpriv->model = ivas_xinitmodel (kpriv, kpriv->modelclass);
    if (kpriv->model == NULL) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "Init ivas_xinitmodel failed for %s", kpriv->modelname.c_str ());
      goto err;
    }

    handle->kernel_priv = (void *) kpriv;
    return true;

  err:
    free (kpriv);
    return -1;
  }

  uint32_t xlnx_kernel_deinit (IVASKernel * handle)
  {
    ivas_xkpriv *kpriv = (ivas_xkpriv *) handle->kernel_priv;
    if (!kpriv)
      return true;
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "enter");

    ivas_perf *pf = &kpriv->pf;

    if (kpriv->performance_test && kpriv->pf.test_started) {
      double time = (get_time () - pf->timer_start) / 1000000.0;
      double fps = (time > 0.0) ? (pf->frames / time) : 999.99;
      printf ("\rframe=%5lu fps=%6.*f        \n", pf->frames,
          (fps < 9.995) ? 3 : 2, fps);
    }
    pf->test_started = 0;
    pf->frames = 0;
    pf->last_displayed_frame = 0;
    pf->timer_start = 0;
    pf->last_displayed_time = 0;

    if (!kpriv->run_time_model) {
      for (int i = 0; i < int (kpriv->mlist.size ()); i++) {
        if (kpriv->mlist[i].model) {
          kpriv->mlist[i].model->close ();
          delete kpriv->mlist[i].model;
          kpriv->mlist[i].model = NULL;
        }
        kpriv->model = NULL;
      }
    }
    kpriv->modelclass = IVAS_XCLASS_NOTFOUND;

    if (kpriv->model != NULL) {
      kpriv->model->close ();
      delete kpriv->model;
      kpriv->model = NULL;
    }
    if (kpriv->labelptr != NULL)
      free (kpriv->labelptr);

    ivas_caps_free (handle);
    free (kpriv);

    return true;
  }

  uint32_t xlnx_kernel_start (IVASKernel * handle, int start,
      IVASFrame * input[MAX_NUM_OBJECT], IVASFrame * output[MAX_NUM_OBJECT])
  {
    ivas_xkpriv *kpriv = (ivas_xkpriv *) handle->kernel_priv;
    ivas_perf *pf = &kpriv->pf;
    GstInferenceMeta *infer_meta = NULL;
    GstIvasInpInferMeta *ivas_inputmeta = NULL;
    IVASFrame *inframe = input[0];
    char *indata = (char *) inframe->vaddr[0];
    int ret, i;

    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "enter");

    if (kpriv->run_time_model) {
      bool found = false;
      ivas_inputmeta =
          gst_buffer_get_ivas_inp_infer_meta ((GstBuffer *) inframe->app_priv);
      if (ivas_inputmeta == NULL) {
        LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
            "error getting ivas_inputmeta");
        return -1;
      }

      kpriv->modelclass = ivas_inputmeta->ml_class;
      kpriv->modelname = ivas_inputmeta->model_name;
      LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level,
          "Runtime model clase is %d", kpriv->modelclass);
      LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level,
          "Runtime model name is %s", kpriv->modelname.c_str ());
      kpriv->elfname = modelexits (kpriv);
      if (kpriv->elfname.empty ()) {
        LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level,
            "Runtime model not found");
        return -1;
      }

      for (i = 0; i < int (kpriv->mlist.size ()); i++) {
        if ((kpriv->mlist[i].modelclass == ivas_inputmeta->ml_class)
            && (kpriv->mlist[i].modelname == ivas_inputmeta->model_name)) {
          LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level,
              "Model already loaded");
          found = true;
          break;
        }
      }

      if (found) {
        kpriv->model = kpriv->mlist[i].model;
        kpriv->labelptr = kpriv->mlist[i].labelptr;
      } else {
        model_list mlist;
        kpriv->model = ivas_xinitmodel (kpriv, kpriv->modelclass);
        if (kpriv->model == NULL) {
          LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
              "Init model failed for %s", kpriv->modelname.c_str ());
          return -1;
        }
        mlist.modelclass = ivas_inputmeta->ml_class;
        mlist.modelname = ivas_inputmeta->model_name;
        mlist.model = kpriv->model;
        mlist.labelptr = kpriv->labelptr;
        kpriv->mlist.push_back (mlist);
      }
    }

    infer_meta = (GstInferenceMeta *) gst_buffer_add_meta ((GstBuffer *)
        inframe->app_priv, gst_inference_meta_get_info (), NULL);
    if (infer_meta == NULL) {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "ivas meta data is not available for dpu");
      return -1;
    } else {
      LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "ivas_mata ptr %p",
          infer_meta);
    }

    cv::Mat image;
    if (input[0]->props.fmt == IVAS_VFMT_BGR8
        || input[0]->props.fmt == IVAS_VFMT_RGB8)
      image =
          cv::Mat (input[0]->props.height, input[0]->props.width, CV_8UC3,
          indata, input[0]->props.stride);
    else {
      LOG_MESSAGE (LOG_LEVEL_ERROR, kpriv->log_level,
          "Not supported format %d\n", input[0]->props.fmt);
      return -1;
    }

    if (kpriv->performance_test && !kpriv->pf.test_started) {
      pf->timer_start = get_time ();
      pf->last_displayed_time = pf->timer_start;
      pf->test_started = 1;
    }

    unsigned int width = kpriv->model->requiredwidth ();
    unsigned int height = kpriv->model->requiredheight ();
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level,
        "model required wxh is %dx%d", width, height);
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "input image wxh is %dx%d",
        inframe->props.width, inframe->props.height);

    if (width != inframe->props.width || height != inframe->props.height) {
      LOG_MESSAGE (LOG_LEVEL_WARNING, kpriv->log_level,
          "Input height/width not match with model" "requirement");
      LOG_MESSAGE (LOG_LEVEL_WARNING, kpriv->log_level,
          "model required wxh is %dx%d", width, height);
      LOG_MESSAGE (LOG_LEVEL_WARNING, kpriv->log_level,
          "input image wxh is %dx%d", inframe->props.width,
          inframe->props.height);
      // return false; //TODO
    }

    ret = ivas_xrunmodel (kpriv, image, infer_meta, inframe);

    if (kpriv->performance_test && kpriv->pf.test_started) {
      pf->frames++;
      if (get_time () - pf->last_displayed_time >= 1000000.0) {
        long long current_time = get_time ();
        double time = (current_time - pf->last_displayed_time) / 1000000.0;
        pf->last_displayed_time = current_time;
        double fps =
            (time >
            0.0) ? ((pf->frames - pf->last_displayed_frame) / time) : 999.99;
        pf->last_displayed_frame = pf->frames;
        printf ("\rframe=%5lu fps=%6.*f        \r", pf->frames,
            (fps < 9.995) ? 3 : 2, fps);
        fflush (stdout);
      }
    }
    //ivas_meta->xmeta.pts = GST_BUFFER_PTS ((GstBuffer *) inframe->app_priv);
    return ret;
  }

  int32_t xlnx_kernel_done (IVASKernel * handle)
  {

    ivas_xkpriv *kpriv = (ivas_xkpriv *) handle->kernel_priv;
    LOG_MESSAGE (LOG_LEVEL_DEBUG, kpriv->log_level, "enter");

    return true;
  }

}
