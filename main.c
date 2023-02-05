#include <jxl.h>
#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> //Header file for sleep(). man 3 sleep for details.
#include <pthread.h>

void* save_(void *vargp)
{

}

// A normal C function that is executed as a thread
// when its name is specified in pthread_create()
void* save_avs3(void *vargp)
{
    /* create encoder */
    h = uavs3e_create(&cfg, NULL);

    if (h == NULL) {
        print_log(0, "cannot create encoder\n");
        return -1;
    }

    tmp_img = image_create(cfg.pic_width, cfg.pic_height, cfg.bit_depth_internal);

    print_config(h, cfg);
    print_stat_header();

    stat.ext_info = enc_ext_info;
    stat.ext_info_buf_size = sizeof(enc_ext_info);

    bitrate = 0;

    /* encode Sequence Header if needed **************************************/
    total_time = 0;
    frame_cnt  = 0;

    /* encode pictures *******************************************************/
	check_time = get_mdate();
    while (1) {
        com_img_t *img_enc = NULL;

        if (finished == 0) {
            /* get encoding buffer */
            if (COM_OK != uavs3e_get_img(h, &img_enc)) {
                print_log(0, "Cannot get original image buffer\n");
                return -1;
            }
            /* read original image */
            if (frame_cnt == frame_to_be_encoded || read_image(fdi, img_enc, cfg.horizontal_size, cfg.vertical_size, cfg.bit_depth_input, cfg.bit_depth_internal, frame_cnt)) {
                print_log(2, "Entering bumping process...\n");
                finished = 1;
                img_enc = NULL;
            } else {
                img_enc->pts = frame_cnt++;
                skip_frames(fdi, img_enc, t_ds_ratio - 1, cfg.bit_depth_input);
            }
        }
        /* encoding */
        time_start = app_get_time();
        stat.insert_idr = 0;

        ret = uavs3e_enc(h, &stat, img_enc);

        time_clk_t now = app_get_time(); 
        time_clk_t time_dur = now - time_start;
        total_time += time_dur;

        /* store bitstream */
        if (ret == COM_OK_OUT_NOT_AVAILABLE) {
            continue;
        } else if (ret == COM_OK) {
            if (fdo > 0 && stat.bytes > 0) {
                _write(fdo, stat.buf, stat.bytes);
            }
            /* get reconstructed image */
            size = sizeof(com_img_t **);
            img_rec = stat.rec_img;
            num_encoded_frames++;

            if (ret < 0) {
                print_log(0, "failed to get reconstruction image\n");
                return -1;
            }

            /* calculate PSNR & SSIM */
            uavs3e_find_psnr(stat.org_img, img_rec, psnr, cfg.bit_depth_internal);
            uavs3e_find_ssim(stat.org_img, img_rec, ssim, cfg.bit_depth_internal);

            /* store reconstructed image to list only for writing out */
            if (fd_rec > 0) {
                cvt_rec_2_output(tmp_img, img_rec, cfg.bit_depth_internal);

                if (write_image(fd_rec, tmp_img, cfg.bit_depth_internal, img_rec->pts)) {
                    print_log(0, "cannot write reconstruction image\n");
                }
            }

            print_psnr(&stat, psnr, ssim, (stat.bytes - stat.user_bytes) << 3, time_dur);

            bitrate += (stat.bytes - stat.user_bytes);

            for (i = 0; i < 3; i++) {
                psnr_avg[i] += psnr[i];
                ssim_avg[i] += ssim[i];
            }

        } else if (ret == COM_OK_NO_MORE_FRM) {
            break;
        } else {
            print_log(2, "uavs3e_enc() err: %d\n", ret);
            return -1;
        }
    }
}


/**
 * Compresses the provided pixels.
 *
 * @param pixels input pixels
 * @param xsize width of the input image
 * @param ysize height of the input image
 * @param compressed will be populated with the compressed bytes
 */
bool EncodeJxlOneshot(const std::vector<float>& pixels, const uint32_t xsize,
                      const uint32_t ysize, std::vector<uint8_t>* compressed) {
  auto enc = JxlEncoderMake(/*memory_manager=*/nullptr);
  auto runner = JxlThreadParallelRunnerMake(
      /*memory_manager=*/nullptr,
      JxlThreadParallelRunnerDefaultNumWorkerThreads());
  if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(enc.get(),
                                                     JxlThreadParallelRunner,
                                                     runner.get())) {
    fprintf(stderr, "JxlEncoderSetParallelRunner failed\n");
    return false;
  }

  JxlPixelFormat pixel_format = {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};

  JxlBasicInfo basic_info;
  JxlEncoderInitBasicInfo(&basic_info);
  basic_info.xsize = xsize;
  basic_info.ysize = ysize;
  basic_info.bits_per_sample = 32;
  basic_info.exponent_bits_per_sample = 8;
  basic_info.uses_original_profile = JXL_FALSE;
  if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(enc.get(), &basic_info)) {
    fprintf(stderr, "JxlEncoderSetBasicInfo failed\n");
    return false;
  }

  JxlColorEncoding color_encoding = {};
  JxlColorEncodingSetToSRGB(&color_encoding,
                            /*is_gray=*/pixel_format.num_channels < 3);
  if (JXL_ENC_SUCCESS !=
      JxlEncoderSetColorEncoding(enc.get(), &color_encoding)) {
    fprintf(stderr, "JxlEncoderSetColorEncoding failed\n");
    return false;
  }

  JxlEncoderFrameSettings* frame_settings =
      JxlEncoderFrameSettingsCreate(enc.get(), nullptr);

  if (JXL_ENC_SUCCESS !=
      JxlEncoderAddImageFrame(frame_settings, &pixel_format,
                              (void*)pixels.data(),
                              sizeof(float) * pixels.size())) {
    fprintf(stderr, "JxlEncoderAddImageFrame failed\n");
    return false;
  }
  JxlEncoderCloseInput(enc.get());

  compressed->resize(64);
  uint8_t* next_out = compressed->data();
  size_t avail_out = compressed->size() - (next_out - compressed->data());
  JxlEncoderStatus process_result = JXL_ENC_NEED_MORE_OUTPUT;
  while (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
    process_result = JxlEncoderProcessOutput(enc.get(), &next_out, &avail_out);
    if (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
      size_t offset = next_out - compressed->data();
      compressed->resize(compressed->size() * 2);
      next_out = compressed->data() + offset;
      avail_out = compressed->size() - offset;
    }
  }
  compressed->resize(next_out - compressed->data());
  if (JXL_ENC_SUCCESS != process_result) {
    fprintf(stderr, "JxlEncoderProcessOutput failed\n");
    return false;
  }

  return true;
}

int read(int argc, char** argv)
{
 struct dirent *dp;
 DIR *dfd;

 char *dir ;
 dir = argv[1] ;

 if ( argc == 1 )
 {
  printf("Usage: %s dirname\n",argv[0]);
  return 0;
 }

 if ((dfd = opendir(dir)) == NULL)
 {
  fprintf(stderr, "Can't open %s\n", dir);
  return 0;
 }

 char filename_qfd[100] ;
 char new_name_qfd[100] ;

 while ((dp = readdir(dfd)) != NULL)
 {
  struct stat stbuf ;
  sprintf( filename_qfd , "%s/%s",dir,dp->d_name) ;
  if( stat(filename_qfd,&stbuf ) == -1 )
  {
   printf("Unable to stat file: %s\n",filename_qfd) ;
   continue ;
  }

  if ( ( stbuf.st_mode & S_IFMT ) == S_IFDIR )
  {
   continue;
   // Skip directories
  }
  else
  {
   char* new_name = get_new_name( dp->d_name ) ;// returns the new string
                                                   // after removing reqd part
   sprintf(new_name_qfd,"%s/%s",dir,new_name) ;
   rename( filename_qfd , new_name_qfd ) ;
  }
 }
}



int main(int argc, char const *argv[])
{
	pthread_t tid_list[8];




	
	pthread_create(&thread_id[i], NULL, myThreadFun, NULL);
	pthread_create(&thread_id[i], NULL, myThreadFun, NULL);
	pthread_create(&thread_id[i], NULL, myThreadFun, NULL);
	pthread_create(&thread_id[i], NULL, myThreadFun, NULL);
	pthread_create(&thread_id[i], NULL, myThreadFun, NULL);
	pthread_create(&thread_id[i], NULL, myThreadFun, NULL);
	pthread_create(&thread_id[i], NULL, myThreadFun, NULL);
	


	pthread_join(thread_id, NULL);
	pthread_join(thread_id, NULL);
	pthread_join(thread_id, NULL);
	pthread_join(thread_id, NULL);
	pthread_join(thread_id, NULL);
	pthread_join(thread_id, NULL);
	pthread_join(thread_id, NULL);
	pthread_join(thread_id, NULL);
	
	return 0;
}
