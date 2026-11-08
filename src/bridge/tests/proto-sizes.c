/* proto-sizes — prints sizes and offsets of the protocol structures. The native output and the
 * PE output (mingw, under Wine) must be identical (ABI test). Owner: capture. */
#include <stddef.h>
#include <stdio.h>
#include "../proto/plh_proto.h"
#define S(t) printf("sizeof %s %u\n", #t, (unsigned)sizeof(struct t))
#define O(t, f) printf("offsetof %s.%s %u\n", #t, #f, (unsigned)offsetof(struct t, f))
int main(void)
{
    S(plh_msg_hdr); O(plh_msg_hdr, seq);
    S(plh_hello); O(plh_hello, token); O(plh_hello, name);
    S(plh_hello_reply); O(plh_hello_reply, daemon_time_ns);
    S(plh_error); S(plh_pong);
    S(plh_cap_output); O(plh_cap_output, name); O(plh_cap_output, description);
    S(plh_cap_output_list); S(plh_cap_open);
    S(plh_cap_open_reply); O(plh_cap_open_reply, slot_offset); O(plh_cap_open_reply, shm_size); O(plh_cap_open_reply, shm_name);
    S(plh_cap_stream_ref); S(plh_rect);
    S(plh_cap_frame); O(plh_cap_frame, frame_id); O(plh_cap_frame, pts_ns); O(plh_cap_frame, dirty_count);
    S(plh_cap_release); S(plh_cap_cursor); O(plh_cap_cursor, pts_ns); O(plh_cap_cursor, hot_y);
    S(plh_cap_state); S(plh_cap_stats); O(plh_cap_stats, frames_captured);
    S(plh_cap_shm_header); O(plh_cap_shm_header, slot_offset); O(plh_cap_shm_header, cursor_max);
    S(plh_vout_create); S(plh_vout_create_reply); O(plh_vout_create_reply, name); S(plh_vout_ref);
    S(plh_display_state); O(plh_display_state, real_right); S(plh_redirect); S(plh_input_redirect_hdr);
    S(plh_input_ev);
    S(plh_input_events_hdr);
    S(plh_input_status);
    S(plh_input_monitor);
    S(plh_input_layout_hdr);
    S(plh_input_layout_reply);
    S(plh_input_gamepad);
    S(plh_enc_caps_reply);
    S(plh_enc_open);
    S(plh_enc_open_reply);
    S(plh_enc_frame);
    S(plh_enc_frame_reply);
    S(plh_enc_reconfig);
    S(plh_enc_session_ref);
    S(plh_enc_shm_header);
    return 0;
}
