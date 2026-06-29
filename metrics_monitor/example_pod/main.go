package main

import (
	"context"
	"errors"
	"fmt"
	"math/rand"
	"net/url"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/exporters/otlp/otlplog/otlploggrpc"
	"go.opentelemetry.io/otel/exporters/otlp/otlpmetric/otlpmetricgrpc"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc"
	otellog "go.opentelemetry.io/otel/log"
	"go.opentelemetry.io/otel/metric"
	"go.opentelemetry.io/otel/propagation"
	sdklog "go.opentelemetry.io/otel/sdk/log"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/trace"
)

const instrumentationName = "metrics-monitor/example-pod"

type config struct {
	serviceName          string
	serviceVersion       string
	environment          string
	namespace            string
	podName              string
	nodeName             string
	otlpEndpoint         string
	otlpInsecure         bool
	emitInterval         time.Duration
	metricExportInterval time.Duration
}

type telemetry struct {
	tracerProvider *sdktrace.TracerProvider
	meterProvider  *sdkmetric.MeterProvider
	loggerProvider *sdklog.LoggerProvider
	tracer         trace.Tracer
	meter          metric.Meter
	logger         otellog.Logger
}

type syntheticRequest struct {
	method string
	route  string
	region string
}

func main() {
	cfg := loadConfig()
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	tel, err := initTelemetry(ctx, cfg)
	if err != nil {
		fmt.Fprintf(os.Stderr, "telemetry setup failed: %v\n", err)
		os.Exit(1)
	}
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if err := tel.Shutdown(shutdownCtx); err != nil {
			fmt.Fprintf(os.Stderr, "telemetry shutdown failed: %v\n", err)
		}
	}()

	fmt.Printf("starting %s; exporting OTLP gRPC telemetry to %s\n", cfg.serviceName, cfg.otlpEndpoint)
	if err := run(ctx, cfg, tel); err != nil && !errors.Is(err, context.Canceled) {
		fmt.Fprintf(os.Stderr, "workload failed: %v\n", err)
		os.Exit(1)
	}
}

func loadConfig() config {
	endpoint, insecureDefault := normalizeEndpoint(firstNonEmpty(
		os.Getenv("OTEL_EXPORTER_OTLP_ENDPOINT"),
		os.Getenv("OTEL_EXPORTER_OTLP_GRPC_ENDPOINT"),
		"otel-collector:4317",
	))

	return config{
		serviceName:          firstNonEmpty(os.Getenv("OTEL_SERVICE_NAME"), os.Getenv("SERVICE_NAME"), "example-otel-pod"),
		serviceVersion:       getenv("SERVICE_VERSION", "0.1.0"),
		environment:          getenv("DEPLOYMENT_ENVIRONMENT", "demo"),
		namespace:            getenv("POD_NAMESPACE", "default"),
		podName:              firstNonEmpty(os.Getenv("POD_NAME"), getenv("HOSTNAME", "example-otel-pod")),
		nodeName:             getenv("NODE_NAME", "unknown"),
		otlpEndpoint:         endpoint,
		otlpInsecure:         boolEnv("OTEL_EXPORTER_OTLP_INSECURE", insecureDefault),
		emitInterval:         durationEnv("EMIT_INTERVAL", 2*time.Second),
		metricExportInterval: durationEnv("METRIC_EXPORT_INTERVAL", 10*time.Second),
	}
}

func initTelemetry(ctx context.Context, cfg config) (*telemetry, error) {
	res, err := resource.New(ctx,
		resource.WithFromEnv(),
		resource.WithTelemetrySDK(),
		resource.WithAttributes(
			attribute.String("service.name", cfg.serviceName),
			attribute.String("service.version", cfg.serviceVersion),
			attribute.String("deployment.environment", cfg.environment),
			attribute.String("k8s.namespace.name", cfg.namespace),
			attribute.String("k8s.pod.name", cfg.podName),
			attribute.String("k8s.node.name", cfg.nodeName),
		),
	)
	if err != nil {
		return nil, fmt.Errorf("create resource: %w", err)
	}

	traceExporter, err := otlptracegrpc.New(ctx, traceExporterOptions(cfg)...)
	if err != nil {
		return nil, fmt.Errorf("create trace exporter: %w", err)
	}

	metricExporter, err := otlpmetricgrpc.New(ctx, metricExporterOptions(cfg)...)
	if err != nil {
		return nil, fmt.Errorf("create metric exporter: %w", err)
	}

	logExporter, err := otlploggrpc.New(ctx, logExporterOptions(cfg)...)
	if err != nil {
		return nil, fmt.Errorf("create log exporter: %w", err)
	}

	tracerProvider := sdktrace.NewTracerProvider(
		sdktrace.WithResource(res),
		sdktrace.WithBatcher(traceExporter),
	)
	meterProvider := sdkmetric.NewMeterProvider(
		sdkmetric.WithResource(res),
		sdkmetric.WithReader(sdkmetric.NewPeriodicReader(metricExporter, sdkmetric.WithInterval(cfg.metricExportInterval))),
	)
	loggerProvider := sdklog.NewLoggerProvider(
		sdklog.WithResource(res),
		sdklog.WithProcessor(sdklog.NewBatchProcessor(logExporter)),
	)

	otel.SetTracerProvider(tracerProvider)
	otel.SetMeterProvider(meterProvider)
	otel.SetTextMapPropagator(propagation.TraceContext{})

	return &telemetry{
		tracerProvider: tracerProvider,
		meterProvider:  meterProvider,
		loggerProvider: loggerProvider,
		tracer:         tracerProvider.Tracer(instrumentationName),
		meter:          meterProvider.Meter(instrumentationName),
		logger:         loggerProvider.Logger(instrumentationName),
	}, nil
}

func run(ctx context.Context, cfg config, tel *telemetry) error {
	requestCounter, err := tel.meter.Int64Counter(
		"demo.requests.total",
		metric.WithDescription("Total synthetic requests handled by the example pod."),
		metric.WithUnit("{request}"),
	)
	if err != nil {
		return fmt.Errorf("create request counter: %w", err)
	}

	errorCounter, err := tel.meter.Int64Counter(
		"demo.request.errors.total",
		metric.WithDescription("Total synthetic failed requests handled by the example pod."),
		metric.WithUnit("{error}"),
	)
	if err != nil {
		return fmt.Errorf("create error counter: %w", err)
	}

	latencyHistogram, err := tel.meter.Float64Histogram(
		"demo.request.duration",
		metric.WithDescription("Synthetic request latency."),
		metric.WithUnit("s"),
	)
	if err != nil {
		return fmt.Errorf("create latency histogram: %w", err)
	}

	queueDepth, err := tel.meter.Int64ObservableGauge(
		"demo.queue.depth",
		metric.WithDescription("Current synthetic checkout queue depth."),
		metric.WithUnit("{item}"),
	)
	if err != nil {
		return fmt.Errorf("create queue gauge: %w", err)
	}

	activeUsers, err := tel.meter.Int64ObservableGauge(
		"demo.active.users",
		metric.WithDescription("Current synthetic active users."),
		metric.WithUnit("{user}"),
	)
	if err != nil {
		return fmt.Errorf("create active user gauge: %w", err)
	}

	var queueDepthValue atomic.Int64
	var activeUsersValue atomic.Int64
	queueDepthValue.Store(12)
	activeUsersValue.Store(80)

	_, err = tel.meter.RegisterCallback(func(ctx context.Context, observer metric.Observer) error {
		observer.ObserveInt64(queueDepth, queueDepthValue.Load(), metric.WithAttributes(attribute.String("queue", "checkout")))
		observer.ObserveInt64(activeUsers, activeUsersValue.Load(), metric.WithAttributes(attribute.String("region", "synthetic-global")))
		return nil
	}, queueDepth, activeUsers)
	if err != nil {
		return fmt.Errorf("register observable metrics callback: %w", err)
	}

	rng := rand.New(rand.NewSource(time.Now().UnixNano()))
	ticker := time.NewTicker(cfg.emitInterval)
	defer ticker.Stop()

	var requestID int64
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			requestID++
			queueDepthValue.Store(int64(5 + rng.Intn(75)))
			activeUsersValue.Store(int64(25 + rng.Intn(250)))
			handleSyntheticRequest(ctx, tel, requestCounter, errorCounter, latencyHistogram, rng, requestID)
		}
	}
}

func handleSyntheticRequest(
	ctx context.Context,
	tel *telemetry,
	requestCounter metric.Int64Counter,
	errorCounter metric.Int64Counter,
	latencyHistogram metric.Float64Histogram,
	rng *rand.Rand,
	requestID int64,
) {
	req := syntheticRequest{
		method: "POST",
		route:  randomChoice(rng, []string{"/checkout", "/cart/apply-coupon", "/inventory/reserve"}),
		region: randomChoice(rng, []string{"us-east", "us-west", "eu-central"}),
	}

	ctx, span := tel.tracer.Start(ctx, "synthetic.request",
		trace.WithSpanKind(trace.SpanKindServer),
		trace.WithAttributes(
			attribute.Int64("request.synthetic_id", requestID),
			attribute.String("http.request.method", req.method),
			attribute.String("http.route", req.route),
			attribute.String("cloud.region", req.region),
		),
	)
	defer span.End()

	simulateDependencySpan(ctx, tel.tracer, rng, "postgres.lookup", "postgresql")
	simulateDependencySpan(ctx, tel.tracer, rng, "redis.reserve_inventory", "redis")

	statusCode := 200
	statusClass := "2xx"
	if rng.Intn(100) < 12 {
		statusCode = 503
		statusClass = "5xx"
		err := errors.New("synthetic inventory service timeout")
		span.RecordError(err)
		span.SetStatus(codes.Error, err.Error())
	} else {
		span.SetStatus(codes.Ok, "synthetic request completed")
	}

	latency := time.Duration(45+rng.Intn(750)) * time.Millisecond
	if statusCode >= 500 {
		latency += 250 * time.Millisecond
	}

	attrs := []attribute.KeyValue{
		attribute.String("http.request.method", req.method),
		attribute.String("http.route", req.route),
		attribute.Int("http.response.status_code", statusCode),
		attribute.String("http.response.status_class", statusClass),
		attribute.String("cloud.region", req.region),
	}
	requestCounter.Add(ctx, 1, metric.WithAttributes(attrs...))
	latencyHistogram.Record(ctx, latency.Seconds(), metric.WithAttributes(attrs...))
	if statusCode >= 500 {
		errorCounter.Add(ctx, 1, metric.WithAttributes(attrs...))
	}

	span.SetAttributes(
		attribute.Int("http.response.status_code", statusCode),
		attribute.Int64("duration_ms", latency.Milliseconds()),
	)
	emitRequestLog(ctx, tel.logger, req, statusCode, latency, requestID)
}

func simulateDependencySpan(ctx context.Context, tracer trace.Tracer, rng *rand.Rand, name string, system string) {
	_, span := tracer.Start(ctx, name,
		trace.WithSpanKind(trace.SpanKindClient),
		trace.WithAttributes(
			attribute.String("db.system", system),
			attribute.Int("db.rows_returned", 1+rng.Intn(20)),
		),
	)
	defer span.End()
	span.AddEvent("dependency call completed")
}

func emitRequestLog(ctx context.Context, logger otellog.Logger, req syntheticRequest, statusCode int, latency time.Duration, requestID int64) {
	record := otellog.Record{}
	record.SetTimestamp(time.Now())
	record.SetObservedTimestamp(time.Now())
	record.SetBody(otellog.StringValue("synthetic request handled"))
	record.SetSeverity(otellog.SeverityInfo)
	record.SetSeverityText("INFO")
	if statusCode >= 500 {
		record.SetBody(otellog.StringValue("synthetic request failed"))
		record.SetSeverity(otellog.SeverityError)
		record.SetSeverityText("ERROR")
	}
	record.AddAttributes(
		otellog.Int64("request.synthetic_id", requestID),
		otellog.String("http.request.method", req.method),
		otellog.String("http.route", req.route),
		otellog.Int("http.response.status_code", statusCode),
		otellog.Int64("duration_ms", latency.Milliseconds()),
		otellog.String("cloud.region", req.region),
	)
	logger.Emit(ctx, record)
}

func (t *telemetry) Shutdown(ctx context.Context) error {
	var err error
	if shutdownErr := t.loggerProvider.Shutdown(ctx); shutdownErr != nil {
		err = errors.Join(err, fmt.Errorf("shutdown logger provider: %w", shutdownErr))
	}
	if shutdownErr := t.meterProvider.Shutdown(ctx); shutdownErr != nil {
		err = errors.Join(err, fmt.Errorf("shutdown meter provider: %w", shutdownErr))
	}
	if shutdownErr := t.tracerProvider.Shutdown(ctx); shutdownErr != nil {
		err = errors.Join(err, fmt.Errorf("shutdown tracer provider: %w", shutdownErr))
	}
	return err
}

func traceExporterOptions(cfg config) []otlptracegrpc.Option {
	options := []otlptracegrpc.Option{otlptracegrpc.WithEndpoint(cfg.otlpEndpoint)}
	if cfg.otlpInsecure {
		options = append(options, otlptracegrpc.WithInsecure())
	}
	return options
}

func metricExporterOptions(cfg config) []otlpmetricgrpc.Option {
	options := []otlpmetricgrpc.Option{otlpmetricgrpc.WithEndpoint(cfg.otlpEndpoint)}
	if cfg.otlpInsecure {
		options = append(options, otlpmetricgrpc.WithInsecure())
	}
	return options
}

func logExporterOptions(cfg config) []otlploggrpc.Option {
	options := []otlploggrpc.Option{otlploggrpc.WithEndpoint(cfg.otlpEndpoint)}
	if cfg.otlpInsecure {
		options = append(options, otlploggrpc.WithInsecure())
	}
	return options
}

func normalizeEndpoint(raw string) (string, bool) {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return "otel-collector:4317", true
	}
	parsed, err := url.Parse(trimmed)
	if err == nil && parsed.Host != "" {
		return parsed.Host, parsed.Scheme != "https"
	}
	return trimmed, true
}

func getenv(key string, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(key)); value != "" {
		return value
	}
	return fallback
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if trimmed := strings.TrimSpace(value); trimmed != "" {
			return trimmed
		}
	}
	return ""
}

func boolEnv(key string, fallback bool) bool {
	value := strings.TrimSpace(os.Getenv(key))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseBool(value)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid %s=%q, using %t\n", key, value, fallback)
		return fallback
	}
	return parsed
}

func durationEnv(key string, fallback time.Duration) time.Duration {
	value := strings.TrimSpace(os.Getenv(key))
	if value == "" {
		return fallback
	}
	parsed, err := time.ParseDuration(value)
	if err != nil || parsed <= 0 {
		fmt.Fprintf(os.Stderr, "invalid %s=%q, using %s\n", key, value, fallback)
		return fallback
	}
	return parsed
}

func randomChoice(rng *rand.Rand, values []string) string {
	return values[rng.Intn(len(values))]
}