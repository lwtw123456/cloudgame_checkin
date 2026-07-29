#pragma once

#ifndef OFFSET_ESTIMATOR_EXTERNAL_STB_IMAGE

#ifndef STB_IMAGE_STATIC
#define STB_IMAGE_STATIC
#define OFFSET_ESTIMATOR_DEFINED_STB_STATIC
#endif

#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define OFFSET_ESTIMATOR_DEFINED_STB_IMPLEMENTATION
#endif

#endif

#include "stb_image.h"

#ifdef OFFSET_ESTIMATOR_DEFINED_STB_IMPLEMENTATION
#undef STB_IMAGE_IMPLEMENTATION
#undef OFFSET_ESTIMATOR_DEFINED_STB_IMPLEMENTATION
#endif

#ifdef OFFSET_ESTIMATOR_DEFINED_STB_STATIC
#undef STB_IMAGE_STATIC
#undef OFFSET_ESTIMATOR_DEFINED_STB_STATIC
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace offset_estimator {

struct OffsetResult {
    double horizontal_offset = 0.0;
    int image_width = 0;
};

namespace detail {

struct DecodedImage {
    stbi_uc* data = nullptr;
    int width = 0;
    int height = 0;
    int source_channels = 0;

    DecodedImage() = default;

    DecodedImage(const DecodedImage&) = delete;
    DecodedImage& operator=(const DecodedImage&) = delete;

    DecodedImage(DecodedImage&& other) noexcept
        : data(other.data),
          width(other.width),
          height(other.height),
          source_channels(other.source_channels) {
        other.data = nullptr;
    }

    DecodedImage& operator=(DecodedImage&& other) noexcept {
        if (this != &other) {
            stbi_image_free(data);

            data = other.data;
            width = other.width;
            height = other.height;
            source_channels = other.source_channels;

            other.data = nullptr;
        }

        return *this;
    }

    ~DecodedImage() {
        stbi_image_free(data);
    }
};

struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<float> pixels;

    float& at(int x, int y) {
        return pixels[
            static_cast<std::size_t>(y) * width + x
        ];
    }

    float at(int x, int y) const {
        return pixels[
            static_cast<std::size_t>(y) * width + x
        ];
    }
};

struct Point {
    int x = 0;
    int y = 0;
};

inline DecodedImage decode(
    std::string_view bytes,
    int desired_channels,
    const char* image_name
) {
    if (bytes.empty()) {
        throw std::invalid_argument(
            std::string(image_name) + "字节数据为空"
        );
    }

    if (bytes.size() >
        static_cast<std::size_t>(
            std::numeric_limits<int>::max()
        )) {
        throw std::invalid_argument(
            std::string(image_name) + "字节数据过大"
        );
    }

    DecodedImage image;

    image.data = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()),
        &image.width,
        &image.height,
        &image.source_channels,
        desired_channels
    );

    if (image.data == nullptr) {
        const char* reason = stbi_failure_reason();

        throw std::runtime_error(
            std::string("无法解码") +
            image_name +
            "：" +
            (reason != nullptr ? reason : "未知错误")
        );
    }

    if (image.width <= 0 || image.height <= 0) {
        throw std::runtime_error(
            std::string(image_name) + "尺寸无效"
        );
    }

    return image;
}

inline int reflect101(int position, int length) {
    if (length <= 1) {
        return 0;
    }

    while (position < 0 || position >= length) {
        if (position < 0) {
            position = -position;
        } else {
            position = 2 * length - position - 2;
        }
    }

    return position;
}

inline GrayImage to_gray(
    const stbi_uc* data,
    int source_width,
    int channels,
    int crop_x0,
    int crop_y0,
    int crop_width,
    int crop_height
) {
    GrayImage result;

    result.width = crop_width;
    result.height = crop_height;
    result.pixels.resize(
        static_cast<std::size_t>(crop_width) *
        crop_height
    );

    for (int y = 0; y < crop_height; ++y) {
        for (int x = 0; x < crop_width; ++x) {
            const int source_x = crop_x0 + x;
            const int source_y = crop_y0 + y;

            const std::size_t index =
                (
                    static_cast<std::size_t>(source_y) *
                    source_width +
                    source_x
                ) *
                channels;

            const float red =
                static_cast<float>(data[index]);

            const float green =
                static_cast<float>(data[index + 1]);

            const float blue =
                static_cast<float>(data[index + 2]);

            result.at(x, y) =
                0.299f * red +
                0.587f * green +
                0.114f * blue;
        }
    }

    return result;
}

inline GrayImage gradient_magnitude(
    const GrayImage& gray
) {

    static constexpr int scharr_x[3][3] = {
        {-3,   0,  3},
        {-10,  0, 10},
        {-3,   0,  3},
    };

    static constexpr int scharr_y[3][3] = {
        {-3, -10, -3},
        { 0,   0,  0},
        { 3,  10,  3},
    };

    // 3×3 高斯核。
    static constexpr int gaussian[3][3] = {
        {1, 2, 1},
        {2, 4, 2},
        {1, 2, 1},
    };

    GrayImage magnitude;

    magnitude.width = gray.width;
    magnitude.height = gray.height;
    magnitude.pixels.resize(gray.pixels.size());

    for (int y = 0; y < gray.height; ++y) {
        for (int x = 0; x < gray.width; ++x) {
            double gradient_x = 0.0;
            double gradient_y = 0.0;

            for (int offset_y = -1;
                 offset_y <= 1;
                 ++offset_y) {
                const int sample_y = reflect101(
                    y + offset_y,
                    gray.height
                );

                for (int offset_x = -1;
                     offset_x <= 1;
                     ++offset_x) {
                    const int sample_x = reflect101(
                        x + offset_x,
                        gray.width
                    );

                    const float value = gray.at(
                        sample_x,
                        sample_y
                    );

                    gradient_x +=
                        value *
                        scharr_x[offset_y + 1][offset_x + 1];

                    gradient_y +=
                        value *
                        scharr_y[offset_y + 1][offset_x + 1];
                }
            }

            magnitude.at(x, y) =
                static_cast<float>(
                    std::hypot(
                        gradient_x,
                        gradient_y
                    )
                );
        }
    }

    GrayImage blurred;

    blurred.width = gray.width;
    blurred.height = gray.height;
    blurred.pixels.resize(gray.pixels.size());

    for (int y = 0; y < gray.height; ++y) {
        for (int x = 0; x < gray.width; ++x) {
            double sum = 0.0;

            for (int offset_y = -1;
                 offset_y <= 1;
                 ++offset_y) {
                const int sample_y = reflect101(
                    y + offset_y,
                    gray.height
                );

                for (int offset_x = -1;
                     offset_x <= 1;
                     ++offset_x) {
                    const int sample_x = reflect101(
                        x + offset_x,
                        gray.width
                    );

                    sum +=
                        magnitude.at(
                            sample_x,
                            sample_y
                        ) *
                        gaussian[
                            offset_y + 1
                        ][
                            offset_x + 1
                        ];
                }
            }

            blurred.at(x, y) =
                static_cast<float>(sum / 16.0);
        }
    }

    return blurred;
}

inline std::size_t count_mask_pixels(
    const std::vector<std::uint8_t>& mask
) {
    return static_cast<std::size_t>(
        std::count_if(
            mask.begin(),
            mask.end(),
            [](std::uint8_t value) {
                return value != 0;
            }
        )
    );
}

inline std::vector<std::uint8_t> erode_mask_3x3(
    const std::vector<std::uint8_t>& mask,
    int width,
    int height
) {
    std::vector<std::uint8_t> eroded(
        mask.size(),
        0
    );

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bool keep = true;

            for (int offset_y = -1;
                 offset_y <= 1 && keep;
                 ++offset_y) {
                const int sample_y = y + offset_y;

                if (sample_y < 0 || sample_y >= height) {
                    continue;
                }

                for (int offset_x = -1;
                     offset_x <= 1;
                     ++offset_x) {
                    const int sample_x = x + offset_x;

                    if (sample_x < 0 ||
                        sample_x >= width) {
                        continue;
                    }

                    const std::size_t index =
                        static_cast<std::size_t>(
                            sample_y
                        ) *
                        width +
                        sample_x;

                    if (mask[index] == 0) {
                        keep = false;
                        break;
                    }
                }
            }

            if (keep) {
                eroded[
                    static_cast<std::size_t>(y) *
                    width +
                    x
                ] = 255;
            }
        }
    }

    return eroded;
}

struct TemplateStats {
    double sum = 0.0;
    double variance = 0.0;
};

inline TemplateStats calculate_template_stats(
    const GrayImage& image,
    const std::vector<Point>& valid_points
) {
    double sum = 0.0;
    double square_sum = 0.0;

    for (const Point& point : valid_points) {
        const double value = image.at(
            point.x,
            point.y
        );

        sum += value;
        square_sum += value * value;
    }

    const double count =
        static_cast<double>(
            valid_points.size()
        );

    TemplateStats result;

    result.sum = sum;
    result.variance =
        square_sum -
        sum * sum / count;

    return result;
}

inline float zncc_at(
    const GrayImage& background,
    const GrayImage& templ,
    const std::vector<Point>& valid_points,
    const TemplateStats& template_stats,
    int match_x,
    int match_y
) {
    double image_sum = 0.0;
    double image_square_sum = 0.0;
    double image_template_sum = 0.0;

    for (const Point& point : valid_points) {
        const double image_value =
            background.at(
                match_x + point.x,
                match_y + point.y
            );

        const double template_value =
            templ.at(
                point.x,
                point.y
            );

        image_sum += image_value;

        image_square_sum +=
            image_value *
            image_value;

        image_template_sum +=
            image_value *
            template_value;
    }

    const double valid_count =
        static_cast<double>(
            valid_points.size()
        );

    const double covariance =
        image_template_sum -
        image_sum *
        template_stats.sum /
        valid_count;

    const double image_variance =
        image_square_sum -
        image_sum *
        image_sum /
        valid_count;

    const double denominator =
        std::sqrt(
            std::max(
                template_stats.variance *
                image_variance,
                0.0
            )
        );

    if (denominator <= 1e-6) {
        return -1.0f;
    }

    return std::clamp(
        static_cast<float>(
            covariance / denominator
        ),
        -1.0f,
        1.0f
    );
}

} // namespace detail

/*
 * 唯一公开调用接口。
 *
 * slice_bytes:
 *     带 Alpha 通道的小图 PNG 字节。
 *
 * background_bytes:
 *     背景图 PNG/JPEG 字节。
 *
 * 可直接传入 cpp-httplib 的：
 *     response->body
 */
inline OffsetResult estimate_offset_from_bytes(
    std::string_view slice_bytes,
    std::string_view background_bytes,
    int alpha_threshold = 30,
    double min_score = 0.35
) {
    if (alpha_threshold < 0 ||
        alpha_threshold > 255) {
        throw std::invalid_argument(
            "alpha_threshold 必须位于 0 到 255 之间"
        );
    }

    if (!std::isfinite(min_score) ||
        min_score < -1.0 ||
        min_score > 1.0) {
        throw std::invalid_argument(
            "min_score 必须位于 -1 到 1 之间"
        );
    }

    detail::DecodedImage piece =
        detail::decode(
            slice_bytes,
            4,
            "小图"
        );

    /*
     * stbi_load_from_memory 即使要求转换为 4 通道，
     * source_channels 仍表示原图的实际通道数。
     */
    if (piece.source_channels != 4) {
        throw std::runtime_error(
            "小图必须是带 Alpha 通道的四通道图片"
        );
    }

    detail::DecodedImage background =
        detail::decode(
            background_bytes,
            3,
            "背景图"
        );

    int crop_x0 = piece.width;
    int crop_y0 = piece.height;
    int crop_x1 = -1;
    int crop_y1 = -1;

    std::size_t visible_pixel_count = 0;

    for (int y = 0; y < piece.height; ++y) {
        for (int x = 0; x < piece.width; ++x) {
            const std::size_t index =
                (
                    static_cast<std::size_t>(y) *
                    piece.width +
                    x
                ) *
                4;

            const std::uint8_t alpha =
                piece.data[index + 3];

            if (alpha > alpha_threshold) {
                ++visible_pixel_count;

                crop_x0 = std::min(crop_x0, x);
                crop_y0 = std::min(crop_y0, y);
                crop_x1 = std::max(crop_x1, x + 1);
                crop_y1 = std::max(crop_y1, y + 1);
            }
        }
    }

    if (visible_pixel_count < 100) {
        throw std::runtime_error(
            "小图中的有效非透明像素过少"
        );
    }

    const int template_width =
        crop_x1 - crop_x0;

    const int template_height =
        crop_y1 - crop_y0;

    if (template_width > background.width ||
        template_height > background.height) {
        throw std::runtime_error(
            "小图的有效区域尺寸大于背景图，无法进行匹配"
        );
    }

    detail::GrayImage template_gray =
        detail::to_gray(
            piece.data,
            piece.width,
            4,
            crop_x0,
            crop_y0,
            template_width,
            template_height
        );

    detail::GrayImage background_gray =
        detail::to_gray(
            background.data,
            background.width,
            3,
            0,
            0,
            background.width,
            background.height
        );

    std::vector<std::uint8_t> mask(
        static_cast<std::size_t>(
            template_width
        ) *
        template_height,
        0
    );

    for (int y = 0; y < template_height; ++y) {
        for (int x = 0; x < template_width; ++x) {
            const int source_x =
                crop_x0 + x;

            const int source_y =
                crop_y0 + y;

            const std::size_t source_index =
                (
                    static_cast<std::size_t>(
                        source_y
                    ) *
                    piece.width +
                    source_x
                ) *
                4;

            if (piece.data[source_index + 3] >= 250) {
                mask[
                    static_cast<std::size_t>(y) *
                    template_width +
                    x
                ] = 255;
            }
        }
    }

    std::vector<std::uint8_t> eroded_mask =
        detail::erode_mask_3x3(
            mask,
            template_width,
            template_height
        );

    if (detail::count_mask_pixels(
            eroded_mask
        ) >= 100) {
        mask = std::move(eroded_mask);
    }

    if (detail::count_mask_pixels(mask) < 50) {
        throw std::runtime_error(
            "用于匹配的有效模板像素过少"
        );
    }

    std::vector<detail::Point> valid_points;

    valid_points.reserve(
        detail::count_mask_pixels(mask)
    );

    for (int y = 0; y < template_height; ++y) {
        for (int x = 0; x < template_width; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y) *
                template_width +
                x;

            if (mask[index] != 0) {
                valid_points.push_back({
                    x,
                    y
                });
            }
        }
    }

    detail::GrayImage template_gradient =
        detail::gradient_magnitude(
            template_gray
        );

    detail::GrayImage background_gradient =
        detail::gradient_magnitude(
            background_gray
        );

    const detail::TemplateStats intensity_stats =
        detail::calculate_template_stats(
            template_gray,
            valid_points
        );

    const detail::TemplateStats gradient_stats =
        detail::calculate_template_stats(
            template_gradient,
            valid_points
        );

    const int result_width =
        background.width -
        template_width +
        1;

    const int result_height =
        background.height -
        template_height +
        1;

    float best_score =
        -std::numeric_limits<float>::infinity();

    int best_match_x = 0;

    for (int match_y = 0;
         match_y < result_height;
         ++match_y) {
        for (int match_x = 0;
             match_x < result_width;
             ++match_x) {
            const float intensity_score =
                detail::zncc_at(
                    background_gray,
                    template_gray,
                    valid_points,
                    intensity_stats,
                    match_x,
                    match_y
                );

            const float gradient_score =
                detail::zncc_at(
                    background_gradient,
                    template_gradient,
                    valid_points,
                    gradient_stats,
                    match_x,
                    match_y
                );

            const float combined_score =
                0.30f * intensity_score +
                0.70f * gradient_score;

            if (combined_score > best_score) {
                best_score = combined_score;
                best_match_x = match_x;
            }
        }
    }

    if (best_score < min_score) {
        throw std::runtime_error(
            "匹配置信度过低：" +
            std::to_string(best_score) +
            " < " +
            std::to_string(min_score)
        );
    }

    return OffsetResult{
        static_cast<double>(
            best_match_x - crop_x0
        ),
        background.width
    };
}

} // namespace offset_estimator
