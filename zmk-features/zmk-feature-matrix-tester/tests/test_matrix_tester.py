import pathlib,subprocess,tempfile,unittest

class MatrixTester(unittest.TestCase):
 def test_actual_observer_and_session_logic(self):
  source=(pathlib.Path(__file__).resolve().parents[1]/'src/matrix_tester.c').read_text()
  logic=source[source.index('#define MATRIX_BYTES'):source.index('static struct zmk_rpc_custom_subsystem_meta')]
  handler=source[source.index('static bool handle_request'):]
  prefix=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define ZMK_KEYMAP_LEN 66
#define ZMK_KEYMAP_SENSORS_LEN 2
#define MAX(a,b) ((a)>(b)?(a):(b))
#define SENSOR_CHAN_ROTATION 1
struct sensor_value {int32_t val1,val2;};
struct zmk_sensor_event {unsigned sensor_index,channel_data_size;struct {struct sensor_value value;unsigned channel;} channel_data[1];};
#define BIT(x) (1U<<(x))
#define BUILD_ASSERT(a,b) _Static_assert(a,b)
struct k_spinlock {int unused;};
typedef int k_spinlock_key_t;
static int k_spin_lock(struct k_spinlock *p){return 0;}
static void k_spin_unlock(struct k_spinlock *p,int k){}
static uint32_t clock_ms;
static uint32_t k_uptime_get_32(void){return clock_ms;}
struct zmk_position_state_changed {unsigned position;bool state;bool physical;struct zmk_sensor_event sensor;};
typedef struct zmk_position_state_changed zmk_event_t;
static const struct zmk_position_state_changed *as_zmk_position_state_changed(const zmk_event_t *e){return e->physical?e:NULL;}
static const struct zmk_sensor_event *as_zmk_sensor_event(const zmk_event_t *e){return e->physical?NULL:&e->sensor;}
static unsigned calls;
int __real_zmk_event_manager_raise(zmk_event_t *event){calls++;return 7;}
typedef struct {unsigned command;} zmk_matrix_tester_Request;
#define zmk_matrix_tester_Request_init_zero {0}
struct bytes {unsigned size;uint8_t bytes[32];};
typedef struct {unsigned key_count;struct bytes pressed,tested;bool active;uint32_t revision;unsigned encoder_count;struct bytes encoder_ticks,encoder_tested;} zmk_matrix_tester_Response;
typedef struct {struct bytes payload;} zmk_custom_CallRequest;
typedef int pb_callback_t;
typedef struct {const uint8_t *data;unsigned size;} pb_istream_t;
static pb_istream_t pb_istream_from_buffer(const uint8_t *p,unsigned n){return (pb_istream_t){p,n};}
#define zmk_matrix_tester_Request_fields 0
static bool pb_decode(pb_istream_t *p,int fields,zmk_matrix_tester_Request *r){if(p->size!=1)return false;r->command=p->data[0];return true;}
static zmk_matrix_tester_Response response;
#define ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(a,b) (&response)
'''
  main=r'''
static void command(unsigned cmd){zmk_custom_CallRequest req={.payload={.size=1,.bytes={cmd}}};assert(handle_request(&req,NULL));}
static void event(unsigned pos,bool down){zmk_event_t ev={pos,down,true};assert(__wrap_zmk_event_manager_raise(&ev)==7);}
static void rotate(unsigned sensor,int val1,int val2,unsigned channel){
 zmk_event_t ev={.physical=false,.sensor={.sensor_index=sensor,.channel_data_size=1,.channel_data={{{val1,val2},channel}}}};
 assert(__wrap_zmk_event_manager_raise(&ev)==7);
}
int main(void){
 event(0,true);command(1);assert(!response.active&&response.pressed.bytes[0]==1&&response.tested.bytes[0]==0);
 command(2);assert(response.active&&response.tested.bytes[0]==1);
 event(0,false);event(8,true);event(8,false);command(1);
 assert(response.pressed.bytes[0]==0&&response.pressed.bytes[1]==0&&response.tested.bytes[0]==1&&response.tested.bytes[1]==1);
 for(unsigned i=0;i<66;i++){event(i,true);event(i,false);}command(1);
 for(unsigned i=0;i<8;i++)assert(response.tested.bytes[i]==255);assert(response.tested.bytes[8]==3);
 event(65,true);command(3);for(unsigned i=0;i<8;i++)assert(response.tested.bytes[i]==0);assert(response.tested.bytes[8]==2);
 event(65,false);command(4);event(3,true);event(3,false);command(1);assert(!response.active&&response.tested.bytes[0]==0);
 command(2);clock_ms+=5001;event(4,true);event(4,false);command(1);assert(!response.active&&response.tested.bytes[0]==0);
 clock_ms=0xfffffff0;command(2);clock_ms=20;event(5,true);event(5,false);command(1);assert(response.active&&response.tested.bytes[0]==32);
 event(66,true);event(400,true);zmk_event_t other={1,true,false};__wrap_zmk_event_manager_raise(&other);command(1);assert(response.tested.bytes[0]==32);
 zmk_custom_CallRequest invalid={.payload={.size=1,.bytes={5}}};assert(!handle_request(&invalid,NULL));
  command(2);rotate(0,24,0,1);rotate(0,0,-1,1);rotate(1,-24,0,1);rotate(1,0,1,1);command(1);
 assert(response.encoder_count==2&&response.encoder_ticks.size==8&&response.encoder_tested.size==1&&response.encoder_tested.bytes[0]==15);
 for(int i=0;i<4;i++)assert(response.encoder_ticks.bytes[i*2]==1);
 rotate(2,1,0,1);rotate(0,0,0,1);rotate(0,1,0,2);command(1);assert(response.encoder_ticks.bytes[0]==1);
 command(3);assert(response.encoder_tested.bytes[0]==0&&response.encoder_ticks.bytes[0]==1);
 rotate(1,24,0,1);command(1);assert(response.encoder_ticks.bytes[4]==2&&response.encoder_tested.bytes[0]==4);
 command(4);rotate(0,24,0,1);command(1);assert(response.encoder_ticks.bytes[0]==1);
 assert(calls>130);puts("Actual source: short taps, all positions, release, clear while held, stop/expiry/wrap, invalid positions and dispatcher preservation passed");
}
'''
  with tempfile.TemporaryDirectory() as temp:
   path=pathlib.Path(temp);(path/'test.c').write_text(prefix+logic+handler+main)
   subprocess.run(['gcc','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
   subprocess.run([str(path/'test')],check=True)

if __name__=='__main__':unittest.main()