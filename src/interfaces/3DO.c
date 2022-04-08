#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"

#include "3DO.h"

#include "read.pio.h"

typedef enum {
  INTERFACE_RESET = 0,
  WAIT_CMD,
  GET_CMD,
  GENERATE_RESULT,
  PUSH_RESULT
} CD_state_t;

uint sm_read = -1;

static int getCmdSize(uint8_t code) {
  switch(code) {
    case 0x83:
      //Read ID / Version
      printf("Get Id\n");
      return 6;
    break;
    default:
      printf("unknown cmd code 0x%x\n", code);
      return 0;
    break;
  }
}

void init_3DO_read_interface(void){
  uint8_t pins[READ_PIN_COUNT] = {CDEN, CDHWR, CDCMD, CDD7, CDD6, CDD5, CDD4, CDD3, CDD2, CDD1, CDD0};
   uint offset = pio_add_program(pio0, &read_program);
   sm_read = pio_claim_unused_sm(pio0, true);
   read_program_init(pio0, sm_read, offset);
}

void core1_entry() {
    init_3DO_read_interface();
    // bool prev_RD_REQ = gpio_get(CDHRD);
    // bool prev_WR_REQ = gpio_get(CDHWR);
    CD_state_t state = INTERFACE_RESET;
    uint8_t request[7];
    // int val = 0;
    // bool oldState = gpio_get(CLK_PIN);
    uint32_t data_in[12];

    printf("Ready\n");
    while (1){
      unsigned int data_all = gpio_get_all();
      bool CD_ACTIVE = (data_all & (1<<CDEN))==0;

      if (CD_ACTIVE) {
        switch(state) {
          case INTERFACE_RESET:
            while( !gpio_get(CDRST)) {
              gpio_put(CDMDCHG, 1); //Under reset
            }
            sleep_ms(150);
            gpio_put(CDMDCHG, 0); //Under reset
            sleep_ms(10);
            gpio_put(CDMDCHG, 1); //Under reset
            sleep_ms(6);
            gpio_put(CDMDCHG, 0); //Under reset
            state = WAIT_CMD;
            printf("wait for msg now\n");
            break;
          case WAIT_CMD:
          for (int i = 0; i<7; i++)
            data_in[i] = pio_sm_get_blocking(pio0, sm_read);
          for (int i = 0; i<7; i++)
            printf("Get %x 0x%x\n", data_in[i], (data_in[i] & 0x3FC));

          break;
          //   // printf("WR %d RD %d\n", WR_REQ, RD_REQ);
          //   if ((WR_EDGE) && (!WR_REQ) && (CMD)) {
          //     request[6] = (data_all>>CDD0) & 0xFF;
          //     data_size = getCmdSize(request[6]);
          //     if (data_size != 0) state = GET_CMD;
          //     else state = GENERATE_RESULT;
          //   }
          //   break;
          // case GET_CMD:
          //   if ((WR_EDGE) && (!WR_REQ) && (CMD)) {
          //     printf("Get a following value\n");
          //     data_size -= 1;
          //     request[data_size] = (data_all>>CDD0) & 0xFF;
          //     if (data_size == 0) state = GENERATE_RESULT;
          //   }
          //   break;
          // case GENERATE_RESULT:
          //   printf("Got Request %x %x %x %x %x %x %x\n", request[6], request[5], request[4], request[3], request[2], request[1], request[0]);
          //   state = PUSH_RESULT;
          //   break;
          // case PUSH_RESULT:
          //   break;
          default:
            break;
      }
    }
  }
}

void _3DO_init() {
  gpio_init_mask(DATA_MASK);
  gpio_init_mask(CTRL_MASK);
  gpio_init_mask(OUTPUT_MASK);
  gpio_set_dir_out_masked(OUTPUT_MASK);

  gpio_put(CDWAIT, 1);  //CDWAIT is always 1
  gpio_put(CDMDCHG, 1); //CDMDCHG is 1 until CD is ready
  gpio_put(CDSTEN, 1); //CDSTEN is 1 when requested status is available and not read
  gpio_put(CDDTEN, 1); //CDDTEN is 1 when requested data is available and not read


  int clock = clock_get_hz(clk_sys);
  printf("Clock is %d\n", clock);

  multicore_launch_core1(core1_entry);
}
