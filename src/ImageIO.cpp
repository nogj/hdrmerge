/*
 *  HDRMerge - HDR exposure merging software.
 *  Copyright 2012 Javier Celaya
 *  jcelaya@gmail.com
 *
 *  This file is part of HDRMerge.
 *
 *  HDRMerge is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  HDRMerge is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with HDRMerge. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <cstdlib>
#include <algorithm>
#include <QImage>
#include <QString>
#include <QRegExp>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <libraw.h>
#include "ImageIO.hpp"
#include "DngFloatWriter.hpp"
#include "Log.hpp"
using namespace std;
using namespace hdrmerge;

Image ImageIO::loadRawImage(const QString& filename, RawParameters & rawParameters, int shot_select) {
    std::unique_ptr<LibRaw> rawProcessor(new LibRaw);
    auto & d = rawProcessor->imgdata;
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 21, 0)
    d.rawparams.shot_select = shot_select;
#else
    d.params.shot_select = shot_select;
#endif
    if (rawProcessor->open_file(rawParameters.fileName.toLocal8Bit().constData()) == LIBRAW_SUCCESS) {
        libraw_decoder_info_t decoder_info;
        rawProcessor->get_decoder_info(&decoder_info);
        if (d.idata.filters <= 1000 && d.idata.filters != 9) {
            Log::msg(Log::DEBUG, "Unsupported filter array (", d.idata.filters, ").");
#ifdef LIBRAW_DECODER_FLATFIELD
        } else if (!(decoder_info.decoder_flags & LIBRAW_DECODER_FLATFIELD)) {
            Log::msg(Log::DEBUG, "LibRaw decoder is not flatfield (", ios::hex, decoder_info.decoder_flags, ").");
#endif
        } else if (rawProcessor->unpack() != LIBRAW_SUCCESS) {
            Log::msg(Log::DEBUG, "LibRaw::unpack() failed.");
        } else {
            rawParameters.fromLibRaw(*(rawProcessor.get()));
        }
    } else {
        Log::msg(Log::DEBUG, "LibRaw::open_file(", rawParameters.fileName, ") failed.");
    }
    return Image(d.rawdata.raw_image, rawParameters, filename);
}

int ImageIO::getFrameCount(RawParameters & rawParameters) {
    std::unique_ptr<LibRaw> rawProcessor(new LibRaw);
    auto & d = rawProcessor->imgdata;
    if (rawProcessor->open_file(rawParameters.fileName.toLocal8Bit().constData()) == LIBRAW_SUCCESS) {
        Log::msg(Log::DEBUG, "Number of frames : ", d.idata.raw_count);
        return d.idata.raw_count;
    } else {
        return 0;
    }

}

ImageIO::QDateInterval ImageIO::getImageCreationInterval(const QString & fileName) {
    std::unique_ptr<LibRaw> rawProcessor(new LibRaw);
    QDateInterval result;
    if (rawProcessor->open_file(fileName.toLocal8Bit().constData()) == LIBRAW_SUCCESS) {
        result.end = QDateTime::fromTime_t(rawProcessor->imgdata.other.timestamp);
        result.start = result.end.addMSecs(-rawProcessor->imgdata.other.shutter * 1000.0);
    }
    return result;
}


int ImageIO::load(const LoadOptions & options, ProgressIndicator & progress) {
    lastError.clear();
    int numImages = options.fileNames.size();
    if (numImages == 0) {
        lastError = QCoreApplication::translate("LoadSave", "No input images were selected.");
        return 0;
    }
    int step;
    int p = 0;
    int error = 0, failedImage = 0;
    stack.clear();
    rawParameters.clear();
    {
        Timer t("Load files");
        if(numImages == 1) { // check for multiframe raw files
            const QString name = options.fileNames[0];
            unique_ptr<RawParameters> params(new RawParameters(name));
            int frameCount = getFrameCount(*params);
            step = 100 / (frameCount + 1);
            p = 0;
            if(frameCount > 0 && frameCount <= 32) {
                // framecount == 1 => create a dng from a single file with a single frame
                // framecount == 2 => create a merged dng from a fuji exr file
                // framecount == 3 => create a merged dng from a pentax hdr file
                for (int i = 0; i < frameCount; ++i) {
                    if (progress.isCanceled()) {
                        lastError = QCoreApplication::translate("LoadSave", "Operation canceled.");
                        stack.clear();
                        rawParameters.clear();
                        return -1;
                    }
                    progress.advance(p, "Loading %1", name.toLocal8Bit().constData());
                    p += step;
                    unique_ptr<RawParameters> params(new RawParameters(name));

                    Image image = loadRawImage(name, *params, i);
                    if (!image.good()) {
                        error = 1;
                        failedImage = i;
                        break;
                    } else if (stack.size() && !params->isSameFormat(*rawParameters.front())) {
                        error = 2;
                        failedImage = i;
                        break;
                    } else {
                        int pos = stack.addImage(std::move(image));
                        rawParameters.emplace_back(std::move(params));
                        for (int j = rawParameters.size() - 1; j > pos; --j)
                            rawParameters[j - 1].swap(rawParameters[j]);
                    }
                }
            } else {
                error = 1;
                lastError = frameCount > 32 ?
                    QCoreApplication::translate("LoadSave", "The RAW contains too many frames.") :
                    QCoreApplication::translate("LoadSave", "The RAW could not be opened or contains no image frames.");
            }
        } else {
            step = 100 / (numImages + 1);
            for (int i = 0; i < numImages; ++i) {
                if (progress.isCanceled()) {
                    lastError = QCoreApplication::translate("LoadSave", "Operation canceled.");
                    stack.clear();
                    rawParameters.clear();
                    return -1;
                }
                const QString name = options.fileNames[i];
                progress.advance(p, "Loading %1", name.toLocal8Bit().constData());
                p += step;
                unique_ptr<RawParameters> params(new RawParameters(name));

                Image image = loadRawImage(name, *params);
                if (!image.good()) {
                    error = 1;
                    failedImage = i;
                    break;
                } else if (stack.size() && !params->isSameFormat(*rawParameters.front())) {
                    error = 2;
                    failedImage = i;
                    break;
                } else {
                    int pos = stack.addImage(std::move(image));
                    rawParameters.emplace_back(std::move(params));
                    for (int j = rawParameters.size() - 1; j > pos; --j)
                        rawParameters[j - 1].swap(rawParameters[j]);
                }
            }
        }
    }
    if (error) {
        if (lastError.isEmpty()) {
            lastError = error == 2 ?
                QCoreApplication::translate("LoadSave", "The selected images do not have the same RAW geometry or CFA pattern.") :
                QCoreApplication::translate("LoadSave", "A RAW image could not be decoded.");
        }
        stack.clear();
        rawParameters.clear();
        return (failedImage << 1) + error - 1;
    }

    progress.advance(p, "Processing stack");

    if (rawParameters.empty() || stack.size() == 0) {
        lastError = QCoreApplication::translate("LoadSave", "No usable RAW frames were loaded.");
        return 0;
    }
    RawParameters & params = *rawParameters.front();
    stack.setFlip(params.flip);
    if(options.useCustomWl)
        // Use custom white level, but only if it's not greater than the value provided by libraw
        params.max = std::min(params.max, options.customWl);
    stack.calculateSaturationLevel(params, options.useCustomWl);
    if (options.align && params.canAlign()) {
        stack.align();
        if (options.crop) {
            stack.crop();
        }
    }
    stack.computeResponseFunctions();
    stack.generateMask(options.deghostThreshold);
    progress.advance(100, "Done loading!");
    return numImages << 1;
}


bool ImageIO::save(const SaveOptions & options, ProgressIndicator & progress) {
    lastError.clear();
    if (stack.size() == 0 || rawParameters.empty()) {
        lastError = QCoreApplication::translate("LoadSave", "There is no HDR image to save.");
        return false;
    }
    if (options.fileName.isEmpty()) {
        lastError = QCoreApplication::translate("LoadSave", "No output file name was specified.");
        return false;
    }

    const QFileInfo outputInfo(options.fileName);
    const QString outputPath = outputInfo.absoluteFilePath();
    for (const auto & input : rawParameters) {
        if (QFileInfo(input->fileName).canonicalFilePath() == outputInfo.canonicalFilePath() ||
            QFileInfo(input->fileName).absoluteFilePath() == outputPath) {
            lastError = QCoreApplication::translate("LoadSave", "The output file cannot overwrite an input image.");
            return false;
        }
    }
    QDir outputDir = outputInfo.absoluteDir();
    if (!outputDir.exists()) {
        lastError = QCoreApplication::translate("LoadSave", "The output directory does not exist.");
        return false;
    }
    const QString nonce = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString temporaryPath = outputDir.filePath("." + outputInfo.fileName() + ".hdrmerge-" + nonce + ".tmp");
    const QString backupPath = outputDir.filePath("." + outputInfo.fileName() + ".hdrmerge-" + nonce + ".bak");
    string cropped = stack.isCropped() ? " cropped" : "";
    Log::msg(2, "Writing ", options.fileName, ", ", options.bps, "-bit, ", stack.getWidth(), 'x', stack.getHeight(), cropped);

    progress.advance(0, "Rendering image");
    RawParameters params = *rawParameters.back();
    params.width = stack.getWidth();
    params.height = stack.getHeight();
    params.adjustWhite(stack.getImage(stack.size() - 1));
    Array2D<float> composedImage = stack.compose(params, options.featherRadius, options.averageSamples,
                                                  options.preserveExposure);
    params.black = params.maxBlack = 0;
    std::fill_n(params.cblack, 4, 0);
    if (progress.isCanceled()) {
        lastError = QCoreApplication::translate("LoadSave", "Operation canceled.");
        return false;
    }

    progress.advance(33, "Rendering preview");
    QImage preview = renderPreview(composedImage, params, stack.getMaxExposure(), options.previewSize <= 1);
    if (progress.isCanceled()) {
        lastError = QCoreApplication::translate("LoadSave", "Operation canceled.");
        return false;
    }

    progress.advance(66, "Writing output");
    DngFloatWriter writer;
    writer.setBitsPerSample(options.bps);
    writer.setPreviewWidth((options.previewSize * stack.getWidth()) / 2);
    writer.setPreview(preview);
    if (!writer.write(std::move(composedImage), params, temporaryPath)) {
        QFile::remove(temporaryPath);
        lastError = QCoreApplication::translate("LoadSave", "The DNG writer failed while creating the output file.");
        return false;
    }
    if (!QFileInfo(temporaryPath).exists() || QFileInfo(temporaryPath).size() == 0) {
        QFile::remove(temporaryPath);
        lastError = QCoreApplication::translate("LoadSave", "The DNG writer produced an empty output file.");
        return false;
    }
    const bool hadOutput = QFileInfo(outputPath).exists();
    if (hadOutput && !QFile::rename(outputPath, backupPath)) {
        QFile::remove(temporaryPath);
        lastError = QCoreApplication::translate("LoadSave", "The existing output file could not be replaced safely.");
        return false;
    }
    if (!QFile::rename(temporaryPath, outputPath)) {
        if (hadOutput) QFile::rename(backupPath, outputPath);
        QFile::remove(temporaryPath);
        lastError = QCoreApplication::translate("LoadSave", "The temporary DNG could not be moved into place.");
        return false;
    }
    if (hadOutput) QFile::remove(backupPath);
    progress.advance(100, "Done writing!");

    if (options.saveMask) {
        QString name = replaceArguments(options.maskFileName, options.fileName);
        if (!saveMaskImage(name)) return false;
    }
    return true;
}


bool ImageIO::saveMaskImage(const QString & maskFile) {
    lastError.clear();
    Log::debug("Saving mask to ", maskFile);
    EditableMask & mask = stack.getMask();
    QImage maskImage(mask.getWidth(), mask.getHeight(), QImage::Format_Indexed8);
    int numColors = stack.size() - 1;
    for (int c = 0; c < numColors; ++c) {
        int gray = (256 * c) / numColors;
        maskImage.setColor(c, qRgb(gray, gray, gray));
    }
    maskImage.setColor(numColors, qRgb(255, 255, 255));
    for (size_t y = 0, pos = 0; y < mask.getHeight(); ++y) {
        for (size_t x = 0; x < mask.getWidth(); ++x, ++pos) {
            maskImage.setPixel(x, y, mask[pos]);
        }
    }
    if (!maskImage.save(maskFile)) {
        lastError = QCoreApplication::translate("LoadSave", "Cannot save the mask image.");
        Log::progress("Cannot save mask image to ", maskFile);
        return false;
    }
    return true;
}


bool ImageIO::loadMaskImage(const QString & maskFile) {
    lastError.clear();
    QImage source(maskFile);
    if (source.isNull()) {
        lastError = QCoreApplication::translate("LoadSave", "Cannot open the mask image.");
        return false;
    }
    EditableMask & target = stack.getMask();
    if (source.width() != static_cast<int>(target.getWidth()) ||
        source.height() != static_cast<int>(target.getHeight())) {
        lastError = QCoreApplication::translate("LoadSave", "The mask dimensions do not match the current image.");
        return false;
    }
    const int maxLayer = static_cast<int>(stack.size()) - 1;
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            int layer;
            if (source.format() == QImage::Format_Indexed8) {
                layer = source.pixelIndex(x, y);
            } else {
                layer = maxLayer > 0 ? (qGray(source.pixel(x, y)) * maxLayer + 127) / 255 : 0;
            }
            target(x, y) = std::max(0, std::min(maxLayer, layer));
        }
    }
    target.reset();
    return true;
}


std::vector<QString> ImageIO::sourceFileNames() const {
    std::vector<QString> files;
    files.reserve(rawParameters.size());
    for (const auto & parameters : rawParameters) files.push_back(parameters->fileName);
    return files;
}


static void prepareRawBuffer(LibRaw & rawProcessor) {
    rawProcessor.imgdata.progress_flags |= LIBRAW_PROGRESS_LOAD_RAW;
    auto & i = rawProcessor.imgdata;
    auto & r = i.rawdata;
    auto & s = i.sizes;
    r.color4_image = nullptr;
    r.color3_image = nullptr;
    size_t numPixels = s.raw_width * (s.raw_height + 7);
    r.raw_alloc = std::malloc(numPixels * sizeof(ushort));
    r.raw_image = (ushort*) r.raw_alloc;
    s.raw_pitch = s.raw_width*2;
    copy_n(&i.color, 1, &r.color);
    copy_n(&i.sizes, 1, &r.sizes);
    copy_n(&i.idata, 1, &r.iparams);
}


QImage ImageIO::renderPreview(const Array2D<float> & rawData, const RawParameters & params, float expShift, bool halfSize) {
    Timer t("Render preview");
    std::unique_ptr<LibRaw> rawProcessor(new LibRaw);
    auto & d = rawProcessor->imgdata;
    d.params.user_sat = 65535;
    d.params.user_black = 0;
    for (int c = 0; c < 4; ++c) {
        d.params.user_cblack[c] = 0;
    }
    d.params.highlight = 2;
    d.params.user_qual = 3;
    d.params.med_passes = 0;
    copy_n(params.camMul, 4, d.params.user_mul);
    d.params.user_flip = 0;
    d.params.exp_correc = 1;
    d.params.exp_shift = expShift;
    d.params.exp_preser = 1.0;
    d.params.half_size = halfSize ? 1 : 0; // much faster, will be used for preview size 'half' or 'none'
    if (rawProcessor->open_file(params.fileName.toLocal8Bit().constData()) == LIBRAW_SUCCESS) {
//             && rawProcessor.unpack() == LIBRAW_SUCCESS) {
        prepareRawBuffer(*(rawProcessor.get()));
        // Assume the other sizes are the same as in the raw parameters
        d.sizes.width = params.width;
        d.sizes.height = params.height;
        float scale = d.params.user_sat / (float)(params.max - params.black);
        for (size_t y = 0; y < params.rawHeight; ++y) {
            for (size_t x = 0; x < params.rawWidth; ++x) {
                size_t pos = y*params.rawWidth + x;
                int v = (rawData[pos] - params.blackAt(x - params.leftMargin, y - params.topMargin)) * scale;
                if (v < 0) v = 0;
                else if (v > 65535) v = 65535;
                d.rawdata.raw_image[pos] = v;
            }
        }
        rawProcessor->dcraw_process();
        libraw_processed_image_t * image = rawProcessor->dcraw_make_mem_image();
        if (image == nullptr) {
            Log::msg(2, "dcraw_make_mem_image() returned NULL");
        } else {
            QImage interpolated(image->width, image->height, QImage::Format_RGB32);
            if (interpolated.isNull()) return QImage();
            for (int y = 0; y < image->height; ++y) {
                QRgb* scanline = (QRgb*)interpolated.scanLine(y);
                int pos = (y*image->width)*3;
                for (int x = 0; x < image->width; ++x) {
                    int r = image->data[pos++], g = image->data[pos++], b = image->data[pos++];
                    scanline[x] = qRgb(r, g, b);
                }
            }
            LibRaw::dcraw_clear_mem(image);
            // The result may be some pixels bigger than the original...
            return interpolated.copy(0, 0, params.width/(halfSize ? 2 : 1 ), params.height/(halfSize ? 2 : 1 ));
        }
    }
    return QImage();
}


class FileNameManipulator {
public:
    FileNameManipulator(const vector<unique_ptr<RawParameters>> & paramList) {
        names.reserve(paramList.size());
        for (auto & rp : paramList) {
            names.push_back(rp->fileName);
        }
        sort(names.begin(), names.end());
    }

    QString getInputBaseName(int i) {
        i = adjustIndex(i);
        if (i == -1) return QString();
        else return getBaseName(names[i]);
    }

    QString getInputBaseNameNoExt(int i) {
        QString name = getInputBaseName(i);
        return name.mid(0, name.lastIndexOf('.'));
    }

    QString getInputDirName(int i) {
        i = adjustIndex(i);
        if (i == -1) return QString();
        else return getDirName(names[i]);
    }

    QString getInputNumberSuffix(int i) {
        QString name = getInputBaseNameNoExt(i);
        int pos = name.length() - 1;
        while (pos >= 0 && name[pos] >= '0' && name[pos] <= '9') pos--;
        return name.mid(pos + 1);
    }

    QString getCommonPrefix() const {
        if (names.empty()) return QString();
        QString prefix = QFileInfo(names.front()).completeBaseName();
        for (size_t i = 1; i < names.size() && !prefix.isEmpty(); ++i) {
            const QString candidate = QFileInfo(names[i]).completeBaseName();
            int common = 0;
            while (common < prefix.length() && common < candidate.length() && prefix[common] == candidate[common]) ++common;
            prefix.truncate(common);
        }
        while (prefix.endsWith('_') || prefix.endsWith('-') || prefix.endsWith(' ')) prefix.chop(1);
        return prefix;
    }

    static QString getBaseName(const QString & name) {
        return QFileInfo(name).fileName();
    }

    static QString getDirName(const QString & name) {
        return QFileInfo(name).canonicalPath();
    }

private:
    vector<QString> names;
    int adjustIndex(int i) {
        if (i < 0)
            i = names.size() + i;
        return i < 0 || i >= (int)names.size() ? -1 : i;
    }
};


QString ImageIO::buildOutputFileName() const {
    if (rawParameters.size() > 1)
        return replaceArguments("%id[-1]/%iF[0]-%in[-1].dng", "");
    else
        return replaceArguments("%id[-1]/%iF[0].dng", "");
}


QString ImageIO::getInputPath() const {
    return FileNameManipulator::getDirName(rawParameters[0]->fileName);
}


QString ImageIO::replaceArguments(const QString & pattern, const QString & outFileName) const {
    QString result(pattern);
    QRegExp re;
    if (outFileName == "") {
        re = QRegExp("%(?:i[fFdn]\\[(-?[0-9]+)\\]|cf|%)");
    } else {
        re = QRegExp("%(?:o[fd]|i[fFdn]\\[(-?[0-9]+)\\]|cf|%)");
    }
    int index = 0;
    FileNameManipulator fnm(rawParameters);
    while ((index = result.indexOf(re, index)) != -1) {
        // What was matched?
        QString token = re.cap();
        if (token[1] == '%') {
            result.replace(index, 2, '%');
        } else if (token.mid(1) == "cf") {
            result.replace(index, 3, fnm.getCommonPrefix());
        } else if (token[1] == 'o') {
            if (token[2] == 'f') {
                result.replace(index, 3, fnm.getBaseName(outFileName));
            } else {
                result.replace(index, 3, fnm.getDirName(outFileName));
            }
        } else { // 'i'
            int imageIndex = re.cap(1).toInt();
            int length = re.cap(1).length() + 5;
            if (token[2] == 'f') {
                result.replace(index, length, fnm.getInputBaseName(imageIndex));
            } else if (token[2] == 'F') {
                result.replace(index, length, fnm.getInputBaseNameNoExt(imageIndex));
            } else if (token[2] == 'd') {
                result.replace(index, length, fnm.getInputDirName(imageIndex));
            } else { // 'n'
                result.replace(index, length, fnm.getInputNumberSuffix(imageIndex));
            }
        }
        index++;
    }
    return result;
}
