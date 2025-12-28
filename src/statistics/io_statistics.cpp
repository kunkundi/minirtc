#include "io_statistics.h"

#include "log.h"

#define STATISTICAL_PERIOD 1000
#define RTP_SEQ_NUM_MAX 65535

namespace minirtc {

IOStatistics::IOStatistics(
    std::function<void(const NetTrafficStats&)> io_report_callback)
    : io_report_callback_(io_report_callback) {
  interval_ = STATISTICAL_PERIOD;
}

IOStatistics::~IOStatistics() { Stop(); }

void IOStatistics::Process() {
  while (running_) {
    std::unique_lock<std::mutex> lock(mtx_);
    cond_var_.wait_for(lock, std::chrono::milliseconds(interval_),
                       [this] { return !running_; });

    video_inbound_bitrate_ = video_inbound_bytes_.load() * 1000 * 8 / interval_;
    video_outbound_bitrate_ =
        video_outbound_bytes_.load() * 1000 * 8 / interval_;
    audio_inbound_bitrate_ = audio_inbound_bytes_.load() * 1000 * 8 / interval_;
    audio_outbound_bitrate_ =
        audio_outbound_bytes_.load() * 1000 * 8 / interval_;
    data_inbound_bitrate_ = data_inbound_bytes_.load() * 1000 * 8 / interval_;
    data_outbound_bitrate_ = data_outbound_bytes_.load() * 1000 * 8 / interval_;
    total_inbound_bitrate_ =
        video_inbound_bitrate_ + audio_inbound_bitrate_ + data_inbound_bitrate_;
    total_outbound_bitrate_ = video_outbound_bitrate_ +
                              audio_outbound_bitrate_ + data_outbound_bitrate_;

    // packet loss rate
    current_period_last_received_video_rtp_pkt_seq_ =
        last_received_video_rtp_pkt_seq_.load();
    expected_video_inbound_rtp_pkt_cnt_ =
        current_period_last_received_video_rtp_pkt_seq_ -
        previous_period_last_received_video_rtp_pkt_seq_ - 1;
    previous_period_last_received_video_rtp_pkt_seq_ =
        current_period_last_received_video_rtp_pkt_seq_;
    if (current_period_last_received_video_rtp_pkt_seq_ > 0 &&
        expected_video_inbound_rtp_pkt_cnt_ > 0) {
      video_rtp_pkt_loss_rate_ = video_rtp_pkt_loss_cnt_.load() /
                                 (float)expected_video_inbound_rtp_pkt_cnt_;
      if (video_rtp_pkt_loss_rate_ > 1.0f) {
        video_rtp_pkt_loss_rate_ = 1.0f;
      }
    } else {
      video_rtp_pkt_loss_rate_ = 0;
    }

    current_period_last_received_audio_rtp_pkt_seq_ =
        last_received_audio_rtp_pkt_seq_.load();
    expected_audio_inbound_rtp_pkt_cnt_ =
        current_period_last_received_audio_rtp_pkt_seq_ -
        previous_period_last_received_audio_rtp_pkt_seq_ - 1;
    previous_period_last_received_audio_rtp_pkt_seq_ =
        current_period_last_received_audio_rtp_pkt_seq_;
    if (current_period_last_received_audio_rtp_pkt_seq_ > 0 &&
        expected_audio_inbound_rtp_pkt_cnt_ > 0) {
      audio_rtp_pkt_loss_rate_ = audio_rtp_pkt_loss_cnt_.load() /
                                 (float)expected_audio_inbound_rtp_pkt_cnt_;
      if (audio_rtp_pkt_loss_rate_ > 1.0f) {
        audio_rtp_pkt_loss_rate_ = 1.0f;
      }
    } else {
      audio_rtp_pkt_loss_rate_ = 0;
    }

    current_period_last_received_data_rtp_pkt_seq_ =
        last_received_data_rtp_pkt_seq_.load();
    expected_data_inbound_rtp_pkt_cnt_ =
        current_period_last_received_data_rtp_pkt_seq_ -
        previous_period_last_received_data_rtp_pkt_seq_ - 1;
    previous_period_last_received_data_rtp_pkt_seq_ =
        current_period_last_received_data_rtp_pkt_seq_;
    if (current_period_last_received_data_rtp_pkt_seq_ > 0 &&
        expected_data_inbound_rtp_pkt_cnt_ > 0) {
      data_rtp_pkt_loss_rate_ = data_rtp_pkt_loss_cnt_.load() /
                                (float)expected_data_inbound_rtp_pkt_cnt_;
      if (data_rtp_pkt_loss_rate_ > 1.0f) {
        data_rtp_pkt_loss_rate_ = 1.0f;
      }
    } else {
      data_rtp_pkt_loss_rate_ = 0;
    }

    video_inbound_bytes_.store(0);
    video_outbound_bytes_.store(0);
    audio_inbound_bytes_.store(0);
    audio_outbound_bytes_.store(0);
    data_inbound_bytes_.store(0);
    data_outbound_bytes_.store(0);
    video_rtp_pkt_loss_cnt_.store(0);
    audio_rtp_pkt_loss_cnt_.store(0);
    data_rtp_pkt_loss_cnt_.store(0);

    if (io_report_callback_) {
      NetTrafficStats net_traffic_stats;
      net_traffic_stats.video_inbound_stats.bitrate = video_inbound_bitrate_;
      net_traffic_stats.video_inbound_stats.rtp_packet_count =
          video_inbound_rtp_pkt_cnt_.load();
      net_traffic_stats.video_inbound_stats.loss_rate =
          video_rtp_pkt_loss_rate_;

      net_traffic_stats.video_outbound_stats.bitrate = video_outbound_bitrate_;
      net_traffic_stats.video_outbound_stats.rtp_packet_count =
          video_outbound_rtp_pkt_cnt_.load();

      net_traffic_stats.audio_inbound_stats.bitrate = audio_inbound_bitrate_;
      net_traffic_stats.audio_inbound_stats.rtp_packet_count =
          audio_inbound_rtp_pkt_cnt_.load();
      net_traffic_stats.audio_inbound_stats.loss_rate =
          audio_rtp_pkt_loss_rate_;

      net_traffic_stats.audio_outbound_stats.bitrate = audio_outbound_bitrate_;
      net_traffic_stats.audio_outbound_stats.rtp_packet_count =
          audio_outbound_rtp_pkt_cnt_.load();

      net_traffic_stats.data_inbound_stats.bitrate = data_inbound_bitrate_;
      net_traffic_stats.data_inbound_stats.rtp_packet_count =
          data_inbound_rtp_pkt_cnt_.load();
      net_traffic_stats.data_inbound_stats.loss_rate = data_rtp_pkt_loss_rate_;

      net_traffic_stats.data_outbound_stats.bitrate = data_outbound_bitrate_;
      net_traffic_stats.data_outbound_stats.rtp_packet_count =
          data_outbound_rtp_pkt_cnt_.load();

      net_traffic_stats.total_inbound_stats.bitrate = total_inbound_bitrate_;
      net_traffic_stats.total_inbound_stats.loss_rate =
          video_rtp_pkt_loss_rate_ + audio_rtp_pkt_loss_rate_ +
          data_rtp_pkt_loss_rate_;
      net_traffic_stats.total_inbound_stats.rtp_packet_count =
          net_traffic_stats.video_inbound_stats.rtp_packet_count +
          net_traffic_stats.audio_inbound_stats.rtp_packet_count +
          net_traffic_stats.data_inbound_stats.rtp_packet_count;

      net_traffic_stats.total_outbound_stats.bitrate = total_outbound_bitrate_;
      net_traffic_stats.total_outbound_stats.rtp_packet_count =
          net_traffic_stats.video_outbound_stats.rtp_packet_count +
          net_traffic_stats.audio_outbound_stats.rtp_packet_count +
          net_traffic_stats.data_outbound_stats.rtp_packet_count;

      io_report_callback_(net_traffic_stats);
    }
  }
}

void IOStatistics::Start() {
  if (!running_) {
    running_ = true;
    statistics_thread_ = std::thread(&IOStatistics::Process, this);
  }
}

void IOStatistics::Stop() {
  running_ = false;
  cond_var_.notify_one();
  if (statistics_thread_.joinable()) {
    statistics_thread_.join();
  }
}

void IOStatistics::UpdateVideoInboundBytes(uint32_t bytes) {
  video_inbound_bytes_ += bytes;
}

void IOStatistics::UpdateVideoOutboundBytes(uint32_t bytes) {
  video_outbound_bytes_ += bytes;
}

void IOStatistics::UpdateVideoPacketLossCount(uint16_t seq_num) {
  const uint16_t HALF_SEQ_RANGE =
      (std::numeric_limits<uint16_t>::max() / 2) + 1;  // 32768
  uint16_t last_v_seq = last_received_video_rtp_pkt_seq_.load();

  if (last_v_seq == 0) {
    last_received_video_rtp_pkt_seq_.store(seq_num);
    return;
  }

  uint16_t diff = seq_num - last_v_seq;
  if (diff == 0) {
    return;
  } else if (diff < HALF_SEQ_RANGE) {
    if (diff > 1) {
      video_rtp_pkt_loss_cnt_ += diff - 1;
    }
    last_received_video_rtp_pkt_seq_.store(seq_num);
  } else {
    if (video_rtp_pkt_loss_cnt_ > 0) {
      video_rtp_pkt_loss_cnt_--;
    }
  }
}

void IOStatistics::UpdateAudioInboundBytes(uint32_t bytes) {
  audio_inbound_bytes_ += bytes;
}

void IOStatistics::UpdateAudioOutboundBytes(uint32_t bytes) {
  audio_outbound_bytes_ += bytes;
}

void IOStatistics::UpdateAudioPacketLossCount(uint16_t seq_num) {
  const uint16_t HALF_SEQ_RANGE =
      (std::numeric_limits<uint16_t>::max() / 2) + 1;  // 32768
  uint16_t last_a_seq = last_received_audio_rtp_pkt_seq_.load();

  if (last_a_seq == 0) {
    last_received_audio_rtp_pkt_seq_.store(seq_num);
    return;
  }

  uint16_t diff = seq_num - last_a_seq;
  if (diff == 0) {
    return;
  } else if (diff < HALF_SEQ_RANGE) {
    if (diff > 1) {
      audio_rtp_pkt_loss_cnt_ += diff - 1;
    }
    last_received_audio_rtp_pkt_seq_.store(seq_num);
  } else {
    if (audio_rtp_pkt_loss_cnt_ > 0) {
      audio_rtp_pkt_loss_cnt_--;
    }
  }
}

void IOStatistics::UpdateDataInboundBytes(uint32_t bytes) {
  data_inbound_bytes_ += bytes;
}

void IOStatistics::UpdateDataOutboundBytes(uint32_t bytes) {
  data_outbound_bytes_ += bytes;
}

void IOStatistics::UpdateDataPacketLossCount(uint16_t seq_num) {
  const uint16_t HALF_SEQ_RANGE =
      (std::numeric_limits<uint16_t>::max() / 2) + 1;  // 32768
  uint16_t last_d_seq = last_received_data_rtp_pkt_seq_.load();

  if (last_d_seq == 0) {
    last_received_data_rtp_pkt_seq_.store(seq_num);
    return;
  }

  uint16_t diff = seq_num - last_d_seq;
  if (diff == 0) {
    return;
  } else if (diff < HALF_SEQ_RANGE) {
    if (diff > 1) {
      data_rtp_pkt_loss_cnt_ += diff - 1;
    }
    last_received_data_rtp_pkt_seq_.store(seq_num);
  } else {
    if (data_rtp_pkt_loss_cnt_ > 0) {
      data_rtp_pkt_loss_cnt_--;
    }
  }
}

void IOStatistics::IncrementVideoInboundRtpPacketCount() {
  ++video_inbound_rtp_pkt_cnt_;
}

void IOStatistics::IncrementVideoOutboundRtpPacketCount() {
  ++video_outbound_rtp_pkt_cnt_;
}

void IOStatistics::IncrementAudioInboundRtpPacketCount() {
  ++audio_inbound_rtp_pkt_cnt_;
}

void IOStatistics::IncrementAudioOutboundRtpPacketCount() {
  ++audio_outbound_rtp_pkt_cnt_;
}

void IOStatistics::IncrementDataInboundRtpPacketCount() {
  ++data_inbound_rtp_pkt_cnt_;
}

void IOStatistics::IncrementDataOutboundRtpPacketCount() {
  ++data_outbound_rtp_pkt_cnt_;
}
}  // namespace minirtc