/*
 * DetectorTrainingApp.cpp
 *
 *  Created on: 23.10.2015
 *      Author: poschmann
 */

#include "stacktrace.hpp"
#include "DetectorTester.hpp"
#include "DetectorTrainer.hpp"
#include "DilatedLabeledImageSource.hpp"
#include "LabeledImage.hpp"
#include "classification/SvmClassifier.hpp"
#include "imageio/DlibImageSource.hpp"
#include "imageprocessing/ChainedFilter.hpp"
#include "imageprocessing/GrayscaleFilter.hpp"
#include "imageprocessing/ImagePyramid.hpp"
#include "imageprocessing/extraction/AggregatedFeaturesExtractor.hpp"
#include "imageprocessing/filtering/AggregationFilter.hpp"
#include "imageprocessing/filtering/FhogAggregationFilter.hpp"
#include "imageprocessing/filtering/FhogFilter.hpp"
#include "imageprocessing/filtering/FpdwFeaturesFilter.hpp"
#include "imageprocessing/filtering/GradientFilter.hpp"
#include "imageprocessing/filtering/GradientHistogramFilter.hpp"
#include "opencv2/highgui/highgui.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>

using cv::Mat;
using cv::Rect;
using cv::Size;
using classification::SvmClassifier;
using detection::AggregatedFeaturesDetector;
using detection::NonMaximumSuppression;
using imageio::DlibImageSource;
using imageio::LabeledImageSource;
using imageprocessing::ChainedFilter;
using imageprocessing::GrayscaleFilter;
using imageprocessing::ImageFilter;
using imageprocessing::ImagePyramid;
using imageprocessing::extraction::AggregatedFeaturesExtractor;
using imageprocessing::filtering::AggregationFilter;
using imageprocessing::filtering::FhogAggregationFilter;
using imageprocessing::filtering::FhogFilter;
using imageprocessing::filtering::FpdwFeaturesFilter;
using imageprocessing::filtering::GradientFilter;
using imageprocessing::filtering::GradientHistogramFilter;
using std::cout;
using std::endl;
using std::invalid_argument;
using std::make_shared;
using std::shared_ptr;
using std::string;
using std::vector;

vector<LabeledImage> getLabeledImages(shared_ptr<LabeledImageSource> source, FeatureParams featureParams) {
	float dilationInPixels = 2.0f;
	float dilationInCells = dilationInPixels / featureParams.cellSizeInPixels;
	float widthScale = (featureParams.windowSizeInCells.width + dilationInCells) / featureParams.windowSizeInCells.width;
	float heightScale = (featureParams.windowSizeInCells.height + dilationInCells) / featureParams.windowSizeInCells.height;
	auto dilatedSource = make_shared<DilatedLabeledImageSource>(source, widthScale, heightScale);
	vector<LabeledImage> images;
	while (dilatedSource->next())
		images.emplace_back(dilatedSource->getImage(), dilatedSource->getLandmarks().getLandmarks());
	return images;
}

shared_ptr<ImageFilter> createFhogFilter(FeatureParams featureParams, bool fast) {
	if (fast) {
		return make_shared<FhogFilter>(featureParams.cellSizeInPixels, 9, false, true, 0.2f);
	} else {
		auto gradientFilter = make_shared<GradientFilter>(1);
		auto gradientHistogramFilter = make_shared<GradientHistogramFilter>(18, false, false, true, false, 0);
		auto aggregationFilter = make_shared<FhogAggregationFilter>(featureParams.cellSizeInPixels, true, 0.2f);
		return make_shared<ChainedFilter>(gradientFilter, gradientHistogramFilter, aggregationFilter);
	}
}

void setFhogFeatures(DetectorTrainer& trainer, FeatureParams featureParams, bool fast) {
	trainer.setFeatures(featureParams, createFhogFilter(featureParams, fast), make_shared<GrayscaleFilter>());
}

shared_ptr<AggregatedFeaturesDetector> loadFhogDetector(const string& filename,
		FeatureParams featureParams, bool fast, shared_ptr<NonMaximumSuppression> nms, int octaveLayerCount, bool approximate) {
	shared_ptr<ImageFilter> fhogFilter = createFhogFilter(featureParams, fast);
	std::ifstream stream(filename);
	shared_ptr<SvmClassifier> svm = SvmClassifier::load(stream);
	stream.close();
	if (approximate) {
		vector<double> lambdas; // TODO
		auto featurePyramid = ImagePyramid::createApproximated(octaveLayerCount, 0.5, 1.0, lambdas);
		featurePyramid->addImageFilter(make_shared<GrayscaleFilter>());
		featurePyramid->addLayerFilter(fhogFilter);
		auto extractor = make_shared<AggregatedFeaturesExtractor>(featurePyramid,
				featureParams.windowSizeInCells, featureParams.cellSizeInPixels, true);
		return make_shared<AggregatedFeaturesDetector>(extractor, svm, nms);
	} else {
		return make_shared<AggregatedFeaturesDetector>(make_shared<GrayscaleFilter>(), fhogFilter,
				featureParams.cellSizeInPixels, featureParams.windowSizeInCells, octaveLayerCount, svm, nms);
	}
}

shared_ptr<ImageFilter> createGradientFeaturesFilter(FeatureParams featureParams) {
	auto gradientFilter = make_shared<GradientFilter>(1);
	int normalizationRadius = featureParams.cellSizeInPixels;
//	auto gradientHistogramFilter = GradientHistogramFilter::full(12, true, normalizationRadius); // 12 + 1
//	auto gradientHistogramFilter = GradientHistogramFilter::half(6, true, normalizationRadius); // 6 + 1
	auto gradientHistogramFilter = GradientHistogramFilter::both(6, true, normalizationRadius); // 12 + 6 + 1
	auto aggregationFilter = make_shared<AggregationFilter>(featureParams.cellSizeInPixels, true, false);
	return make_shared<ChainedFilter>(gradientFilter, gradientHistogramFilter, aggregationFilter);
}

void setGradientFeatures(DetectorTrainer& trainer, FeatureParams featureParams) {
	trainer.setFeatures(featureParams, createGradientFeaturesFilter(featureParams), make_shared<GrayscaleFilter>());
}

shared_ptr<AggregatedFeaturesDetector> loadGradientFeaturesDetector(const string& filename,
		FeatureParams featureParams, shared_ptr<NonMaximumSuppression> nms, int octaveLayerCount, bool approximate) {
	shared_ptr<ImageFilter> gradientFeaturesFilter = createGradientFeaturesFilter(featureParams);
	std::ifstream stream(filename);
	shared_ptr<SvmClassifier> svm = SvmClassifier::load(stream);
	stream.close();
	if (approximate) {
		vector<double> lambdas; // TODO
		auto featurePyramid = ImagePyramid::createApproximated(octaveLayerCount, 0.5, 1.0, lambdas);
		featurePyramid->addImageFilter(make_shared<GrayscaleFilter>());
		featurePyramid->addLayerFilter(gradientFeaturesFilter);
		auto extractor = make_shared<AggregatedFeaturesExtractor>(featurePyramid,
				featureParams.windowSizeInCells, featureParams.cellSizeInPixels, true);
		return make_shared<AggregatedFeaturesDetector>(extractor, svm, nms);
	} else {
		return make_shared<AggregatedFeaturesDetector>(make_shared<GrayscaleFilter>(), gradientFeaturesFilter,
				featureParams.cellSizeInPixels, featureParams.windowSizeInCells, octaveLayerCount, svm, nms);
	}
}

shared_ptr<ImageFilter> createFdpwFilter(FeatureParams featureParams) {
	auto fpdwFeatures = make_shared<FpdwFeaturesFilter>(true, false, featureParams.cellSizeInPixels, 0.01);
	auto aggregation = make_shared<AggregationFilter>(featureParams.cellSizeInPixels, true, false);
	return make_shared<ChainedFilter>(fpdwFeatures, aggregation);
}

void setFpdwFeatures(DetectorTrainer& trainer, FeatureParams featureParams) {
	trainer.setFeatures(featureParams, createFdpwFilter(featureParams));
}

shared_ptr<AggregatedFeaturesDetector> loadFpdwDetector(const string& filename,
		FeatureParams featureParams, shared_ptr<NonMaximumSuppression> nms, int octaveLayerCount, bool approximate) {
	shared_ptr<ImageFilter> filter = createFdpwFilter(featureParams);
	std::ifstream stream(filename);
	shared_ptr<SvmClassifier> svm = SvmClassifier::load(stream);
	stream.close();
	if (approximate) {
		vector<double> lambdas; // TODO
		auto featurePyramid = ImagePyramid::createApproximated(octaveLayerCount, 0.5, 1.0, lambdas);
		featurePyramid->addLayerFilter(filter);
		auto extractor = make_shared<AggregatedFeaturesExtractor>(featurePyramid,
				featureParams.windowSizeInCells, featureParams.cellSizeInPixels, true);
		return make_shared<AggregatedFeaturesDetector>(extractor, svm, nms);
	} else {
		return make_shared<AggregatedFeaturesDetector>(filter,
				featureParams.cellSizeInPixels, featureParams.windowSizeInCells, octaveLayerCount, svm, nms);
	}
}

void showDetections(const DetectorTester& tester, AggregatedFeaturesDetector& detector, vector<LabeledImage>& images) {
	Mat output;
	cv::Scalar correctDetectionColor(0, 255, 0);
	cv::Scalar wrongDetectionColor(0, 0, 255);
	cv::Scalar ignoredDetectionColor(255, 204, 0);
	cv::Scalar missedDetectionColor(0, 153, 255);
	int thickness = 2;
	for (const LabeledImage& image : images) {
		DetectionResult result = tester.detect(detector, image.image, image.landmarks);
		image.image.copyTo(output);
		for (const Rect& target : result.correctDetections)
			cv::rectangle(output, target, correctDetectionColor, thickness);
		for (const Rect& target : result.wrongDetections)
			cv::rectangle(output, target, wrongDetectionColor, thickness);
		for (const Rect& target : result.ignoredDetections)
			cv::rectangle(output, target, ignoredDetectionColor, thickness);
		for (const Rect& target : result.missedDetections)
			cv::rectangle(output, target, missedDetectionColor, thickness);
		cv::imshow("Detections", output);
		int key = cv::waitKey(0);
		if (static_cast<char>(key) == 'q')
			break;
	}
}

void printTestResult(DetectorEvaluationResult result) {
	cout << "F-Measure: " << result.getF1Measure() << endl;
	cout << "Precision: " << result.getPrecision() << endl;
	cout << "Recall: " << result.getRecall() << endl;
	cout << "True positives: " << result.getTruePositives() << endl;
	cout << "False positives: " << result.getFalsePositives() << endl;
	cout << "False negatives: " << result.getFalseNegatives() << endl;
	cout << "Average time: " << result.getAverageDetectionDuration().count() << " ms" << endl;
}

void printTestResult(const string& title, DetectorEvaluationResult result) {
	cout << "=== " << title << " ===" << endl;
	printTestResult(result);
}

enum class TaskType { TRAIN, TEST, TEST_APPROXIMATE };
enum class FeatureType { FHOG, FAST_FHOG, GRADHIST, FPDW };

TaskType getTaskType(const string& type) {
	if (type == "train")
		return TaskType::TRAIN;
	if (type == "test")
		return TaskType::TEST;
	if (type == "test-approximate")
		return TaskType::TEST_APPROXIMATE;
	throw invalid_argument("expected train/test/test-approximate, but was '" + type + "'");
}

FeatureType getFeatureType(const string& type) {
	if (type == "fhog")
		return FeatureType::FHOG;
	if (type == "fastfhog")
		return FeatureType::FAST_FHOG;
	if (type == "gradhist")
		return FeatureType::GRADHIST;
	if (type == "fpdw")
		return FeatureType::FPDW;
	throw invalid_argument("expected fhog/gradhist/fpdw, but was '" + type + "'");
}

int main(int argc, char** argv) {
	if (argc != 4) {
		cout << "call: ./DetectorTrainingApp train/test/test-approximate fhog/gradhist/fpdw svmfile" << endl;
		return 0;
	}
	TaskType taskType = getTaskType(argv[1]);
	FeatureType featureType = getFeatureType(argv[2]);
	string filename = argv[3];

	TrainingParams trainingParams;
	trainingParams.negativeScoreThreshold = -0.5;
	trainingParams.overlapThreshold = 0.3;
	trainingParams.C = 10;
	trainingParams.compensateImbalance = true;
	FeatureParams featureParams{Size(5, 7), 7, 10}; // (width, height), cellsize, octave
	int octaveLayerCountForDetection = 5;
	shared_ptr<NonMaximumSuppression> nms = make_shared<NonMaximumSuppression>(0.3, NonMaximumSuppression::MaximumType::WEIGHTED_AVERAGE);

	vector<LabeledImage> trainingImages = getLabeledImages(make_shared<DlibImageSource>("training.xml"), featureParams);
	vector<LabeledImage> testingImages = getLabeledImages(make_shared<DlibImageSource>("testing.xml"), featureParams);

	shared_ptr<AggregatedFeaturesDetector> detector;
	if (taskType == TaskType::TRAIN) {
		cout << "train and test detector" << endl;
		DetectorTrainer trainer(true);
		trainer.setTrainingParameters(trainingParams);
		if (featureType == FeatureType::FHOG)
			setFhogFeatures(trainer, featureParams, false);
		else if (featureType == FeatureType::FAST_FHOG)
			setFhogFeatures(trainer, featureParams, true);
		else if (featureType == FeatureType::GRADHIST)
			setGradientFeatures(trainer, featureParams);
		else if (featureType == FeatureType::FPDW)
			setFpdwFeatures(trainer, featureParams);
		trainer.train(trainingImages);
		trainer.storeClassifier(filename);
		detector = trainer.getDetector(nms, octaveLayerCountForDetection);
	} else {
		bool approximate = taskType == TaskType::TEST_APPROXIMATE;
		cout << "test detector";
		if (approximate)
			cout << " on approximated image pyramid";
		cout << endl;
		if (featureType == FeatureType::FHOG)
			detector = loadFhogDetector(filename, featureParams, false, nms, octaveLayerCountForDetection, approximate);
		else if (featureType == FeatureType::FAST_FHOG)
			detector = loadFhogDetector(filename, featureParams, true, nms, octaveLayerCountForDetection, approximate);
		else if (featureType == FeatureType::GRADHIST)
			detector = loadGradientFeaturesDetector(filename, featureParams, nms, octaveLayerCountForDetection, approximate);
		else if (featureType == FeatureType::FPDW)
			detector = loadFpdwDetector(filename, featureParams, nms, octaveLayerCountForDetection, approximate);
	}

	DetectorTester tester;
	printTestResult("Evaluation on training set", tester.evaluate(*detector, trainingImages));
	printTestResult("Evaluation on testing set", tester.evaluate(*detector, testingImages));
	showDetections(tester, *detector, testingImages);

	return 0;
}
