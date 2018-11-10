#include "nbody_engine_opencl.h"
#include <QDebug>
#include <QFile>
#include <QStringList>
#include <set>

#ifdef HAVE_OPENCL2
#define CL_HPP_ENABLE_EXCEPTIONS
#define CL_USE_DEPRECATED_OPENCL_2_0_APIS
#define CL_HPP_ENABLE_PROGRAM_CONSTRUCTION_FROM_ARRAY_COMPATIBILITY
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#include <CL/cl2.hpp>
namespace cl {
using namespace compatibility;
}
#else //HAVE_OPENCL2
#define __CL_ENABLE_EXCEPTIONS
#define CL_HPP_ENABLE_EXCEPTIONS
#define CL_USE_DEPRECATED_OPENCL_2_0_APIS
#include <CL/cl.hpp>
#endif //HAVE_OPENCL2

std::string load_program(const QString& filename)
{
	QFile			file(filename);
	if(!file.open(QFile::ReadOnly))
	{
		return std::string();
	}
	std::string	src(file.readAll().data());
	return src;
}

cl::Program load_programs(const cl::Context& context, const cl::Device& device, const QString& options,
						  const QStringList& files)
{
	std::vector< std::string >	source_data;
	cl::Program::Sources		sources;

	for(int i = 0; i != files.size(); ++i)
	{
		source_data.push_back(load_program(files[i]));
		sources.push_back(std::make_pair(source_data.back().data(), source_data.back().size()));
	}

	cl::Program	prog(context, sources);

	try
	{
		std::vector<cl::Device>	devices;
		devices.push_back(device);
		prog.build(devices, options.toLatin1());
	}
	catch(cl::Error& err)
	{
		qDebug() << prog.getBuildInfo<CL_PROGRAM_BUILD_LOG>(device).data();
		throw;
	}
	catch(...)
	{
		throw std::exception();
	}
	return prog;
}

typedef cl::make_kernel< cl_int, cl_int,	//Block offset
		cl::Buffer,	//mass
		cl::Buffer,	//y
		cl::Buffer,	//f
		cl_int, cl_int,	//yoff,foff
		cl_int, cl_int	//points_count,stride
		> ComputeBlock;

typedef cl::make_kernel< cl::Buffer, const nbcoord_t > FMfill;
typedef cl::make_kernel< cl_int, cl::Buffer, cl::Buffer, const nbcoord_t > FMadd1;
typedef cl::make_kernel< cl::Buffer, cl::Buffer, cl::Buffer, const nbcoord_t, cl_int, cl_int, cl_int > FMadd2;
typedef cl::make_kernel< cl::Buffer, cl_int, cl_int, cl::Buffer, cl_int > FMaxabs;

struct nbody_engine_opencl::data
{
	struct devctx
	{
		cl::Context			m_context;
		cl::Program			m_prog;
		cl::CommandQueue	m_queue;
		ComputeBlock		m_fcompute;
		FMfill				m_fill;
		FMadd1				m_fmadd1;
		FMadd2				m_fmadd2;
		FMaxabs				m_fmaxabs;

		static QString build_options();
		static QStringList sources();
		devctx(cl::Context& context, cl::Device& device);
	};
	cl::Context				m_context;
	std::vector<devctx>		m_devices;
	smemory*				m_mass;
	smemory*				m_y;
	nbody_data*				m_data;

	int select_devices(const QString& devices);
	void prepare(devctx&, const nbody_data* data, const nbvertex_t* vertites);
	void compute_block(devctx& ctx, size_t offset_n1, size_t offset_n2, const nbody_data* data);
};

class nbody_engine_opencl::smemory : public nbody_engine::memory
{
	size_t							m_size;
	std::vector<cl::Buffer>			m_buffers;
public:
	smemory(size_t size, const std::vector<data::devctx>& dev) : m_size(size)
	{
		m_buffers.reserve(dev.size());
		for(size_t i = 0; i != dev.size(); ++i)
		{
			m_buffers.emplace_back(dev[i].m_context, CL_MEM_READ_WRITE, alloc_size());
		}
	}

	size_t size() const override
	{
		return m_size;
	}

	size_t alloc_size() const
	{
		constexpr	size_t block_size = sizeof(nbcoord_t) * NBODY_DATA_BLOCK_SIZE;
		return block_size * (1 + (m_size - 1) / block_size);
	}

	const cl::Buffer& buffer(size_t i) const
	{
		return m_buffers[i];
	}

	cl::Buffer& buffer(size_t i)
	{
		return m_buffers[i];
	}
};

QString nbody_engine_opencl::data::devctx::build_options()
{
	QString			options;

	options += "-DNBODY_DATA_BLOCK_SIZE=" + QString::number(NBODY_DATA_BLOCK_SIZE) + " ";
	options += "-DNBODY_MIN_R=" + QString::number(NBODY_MIN_R) + " ";
	options += "-Dnbcoord_t=" + QString(nbtype_info<nbcoord_t>::type_name()) + " ";
	return options;
}

QStringList nbody_engine_opencl::data::devctx::sources()
{
	return QStringList() << ":/nbody_engine_opencl.cl";
}

nbody_engine_opencl::data::devctx::devctx(cl::Context& _context, cl::Device& device) :
	m_context(_context),
	m_prog(load_programs(m_context, device, build_options(), sources())),
	m_queue(m_context, device, 0),
	m_fcompute(m_prog, "ComputeBlockLocal"),
	m_fill(m_prog, "fill"),
	m_fmadd1(m_prog, "fmadd1"),
	m_fmadd2(m_prog, "fmadd2"),
	m_fmaxabs(m_prog, "fmaxabs")
{
	size_t	local_memory_amount = 0;

	local_memory_amount += NBODY_DATA_BLOCK_SIZE * 4 * sizeof(nbcoord_t); // x2, y2, z2, m2

	qDebug() << "\t\t\tKernel local memory" << local_memory_amount / 1024.0 << "Kb";
}

int nbody_engine_opencl::data::select_devices(const QString& devices)
{
	std::vector<cl::Platform>		platforms;
	try
	{
		cl::Platform::get(&platforms);
	}
	catch(cl::Error& e)
	{
		qDebug() << e.err() << e.what();
		return -1;
	}

	QStringList	platform_devices_list(devices.split(";"));

	for(int n = 0; n != platform_devices_list.size(); ++n)
	{
		QStringList	plat_dev(platform_devices_list[n].split(":"));
		if(plat_dev.size() != 2)
		{
			qDebug() << "Can't parse OpenCL platform-device pair";
			return -1;
		}

		bool	platform_id_ok = false;
		size_t	platform_n = static_cast<size_t>(plat_dev[0].toUInt(&platform_id_ok));

		if(!platform_id_ok)
		{
			qDebug() << "Can't parse OpenCL platform-device pair";
			return -1;
		}

		if(platform_n >= platforms.size())
		{
			qDebug() << "Platform #" << platform_n << "not found. Max platform ID is" << platforms.size() - 1;
			return -1;
		}

		QStringList					devices_list(plat_dev[1].split(","));
		const cl::Platform&			platform(platforms[platform_n]);
		std::vector<cl::Device>		all_devices;
		std::vector<cl::Device>		active_devices;
		std::set<size_t>			dev_ids;

		platform.getDevices(CL_DEVICE_TYPE_ALL, &all_devices);
		qDebug() << platform_n << platform.getInfo<CL_PLATFORM_VENDOR>().c_str();

		for(int i = 0; i != devices_list.size(); ++i)
		{
			bool	device_id_ok = false;
			size_t	device_n = static_cast<size_t>(devices_list[i].toUInt(&device_id_ok));
			if(!device_id_ok)
			{
				qDebug() << "Can't parse OpenCL device ID" << devices_list[i];
				return -1;
			}
			if(device_n >= all_devices.size())
			{
				qDebug() << "Device #" << device_n << "not found. Max device ID is" << all_devices.size() - 1;
				return -1;
			}
			active_devices.push_back(all_devices[device_n]);
			dev_ids.insert(device_n);
		}

		std::vector<cl::Device>		context_devices;
		for(auto ii = dev_ids.begin(); ii != dev_ids.end(); ++ii)
		{
			context_devices.push_back(all_devices[*ii]);
		}

		cl::Context	context(context_devices);

		for(size_t device_n = 0; device_n != active_devices.size(); ++device_n)
		{
			cl::Device&		device(active_devices[device_n]);
			qDebug() << "\tDevice:";
			qDebug() << "\t\t CL_DEVICE_NAME" << device.getInfo<CL_DEVICE_NAME>().c_str();
			qDebug() << "\t\t CL_DEVICE_VERSION" << device.getInfo<CL_DEVICE_VERSION>().c_str();
			m_devices.push_back(devctx(context, device));
		}
	}

	if(m_devices.empty())
	{
		qDebug() << "No OpenCL device found";
		return -1;
	}

	return 0;
}

nbody_engine_opencl::nbody_engine_opencl() :
	d(new data())
{
}

nbody_engine_opencl::~nbody_engine_opencl()
{
	delete d;
}

const char* nbody_engine_opencl::type_name() const
{
	return "nbody_engine_opencl";
}

void nbody_engine_opencl::init(nbody_data* body_data)
{
	if(d->m_devices.empty())
	{
		qDebug() << "No OpenCL device available";
		return;
	}
	d->m_data = body_data;
	d->m_mass = dynamic_cast<smemory*>(create_buffer(sizeof(nbcoord_t) * d->m_data->get_count()));
	d->m_y = dynamic_cast<smemory*>(create_buffer(sizeof(nbcoord_t) * problem_size()));

	std::vector<nbcoord_t>	ytmp(problem_size());
	size_t					count = d->m_data->get_count();
	nbcoord_t*				rx = ytmp.data();
	nbcoord_t*				ry = rx + count;
	nbcoord_t*				rz = rx + 2 * count;
	nbcoord_t*				vx = rx + 3 * count;
	nbcoord_t*				vy = rx + 4 * count;
	nbcoord_t*				vz = rx + 5 * count;
	const nbvertex_t*		vrt = d->m_data->get_vertites();
	const nbvertex_t*		vel = d->m_data->get_velosites();

	for(size_t i = 0; i != count; ++i)
	{
		rx[i] = vrt[i].x;
		ry[i] = vrt[i].y;
		rz[i] = vrt[i].z;
		vx[i] = vel[i].x;
		vy[i] = vel[i].y;
		vz[i] = vel[i].z;
	}

	write_buffer(d->m_mass, const_cast<nbcoord_t*>(d->m_data->get_mass()));
	write_buffer(d->m_y, ytmp.data());
}

void nbody_engine_opencl::get_data(nbody_data* body_data)
{
	if(d->m_devices.empty())
	{
		qDebug() << "No OpenCL device available";
		return;
	}

	std::vector<nbcoord_t>	ytmp(problem_size());
	size_t					count = body_data->get_count();
	const nbcoord_t*		rx = ytmp.data();
	const nbcoord_t*		ry = rx + count;
	const nbcoord_t*		rz = rx + 2 * count;
	const nbcoord_t*		vx = rx + 3 * count;
	const nbcoord_t*		vy = rx + 4 * count;
	const nbcoord_t*		vz = rx + 5 * count;
	nbvertex_t*				vrt = body_data->get_vertites();
	nbvertex_t*				vel = body_data->get_velosites();

	read_buffer(ytmp.data(), d->m_y);

	for(size_t i = 0; i != count; ++i)
	{
		vrt[i].x = rx[i];
		vrt[i].y = ry[i];
		vrt[i].z = rz[i];
		vel[i].x = vx[i];
		vel[i].y = vy[i];
		vel[i].z = vz[i];
	}
}

size_t nbody_engine_opencl::problem_size() const
{
	if(d->m_data == NULL)
	{
		return 0;
	}
	return 6 * d->m_data->get_count();
}

nbody_engine::memory* nbody_engine_opencl::get_y()
{
	return d->m_y;
}

void nbody_engine_opencl::advise_time(const nbcoord_t& dt)
{
	d->m_data->advise_time(dt);
}

nbcoord_t nbody_engine_opencl::get_time() const
{
	return d->m_data->get_time();
}

void nbody_engine_opencl::set_time(nbcoord_t t)
{
	d->m_data->set_time(t);
}

size_t nbody_engine_opencl::get_step() const
{
	return d->m_data->get_step();
}

void nbody_engine_opencl::set_step(size_t s)
{
	d->m_data->set_step(s);
}

void nbody_engine_opencl::fcompute(const nbcoord_t& t, const memory* _y, memory* _f)
{
	if(d->m_devices.empty())
	{
		qDebug() << "No OpenCL device available";
		return;
	}

	Q_UNUSED(t);
	advise_compute_count();

	const smemory*	y = dynamic_cast<const smemory*>(_y);
	smemory*		f = dynamic_cast<smemory*>(_f);

	if(y == NULL)
	{
		qDebug() << "y is not smemory";
		return;
	}
	if(f == NULL)
	{
		qDebug() << "f is not smemory";
		return;
	}

	if(d->m_devices.empty())
	{
		qDebug() << Q_FUNC_INFO << "m_devices.empty()";
		return;
	}

	size_t	device_count(d->m_devices.size());

	if(device_count > 1)
	{
		// synchronize multiple devices
		copy_buffer(y, y);
	}

	{
		size_t					data_size = d->m_data->get_count();
		size_t					device_data_size = data_size / device_count;
		cl::NDRange				global_range(device_data_size);
		cl::NDRange				local_range(NBODY_DATA_BLOCK_SIZE);
		std::vector<cl::Event>	events;

		for(size_t dev_n = 0; dev_n != device_count; ++dev_n)
		{
			size_t			offset = dev_n * device_data_size;
			data::devctx&	ctx(d->m_devices[dev_n]);
			cl::EnqueueArgs	eargs(ctx.m_queue, global_range, local_range);
			cl::Event		ev(ctx.m_fcompute(eargs, offset, 0, d->m_mass->buffer(dev_n),
											  y->buffer(dev_n), f->buffer(dev_n), 0, 0,
											  d->m_data->get_count(), d->m_data->get_count()));
			events.push_back(ev);
		}

		cl::Event::waitForEvents(events);
	}

	if(device_count > 1)
	{
		// synchronize again
		QByteArray		host_buffer(static_cast<int>(f->size()), Qt::Uninitialized);
		size_t			data_size = f->size();
		size_t			row_count = 6;
		size_t			device_data_size = data_size / device_count;
		size_t			row_size = data_size / row_count;
		size_t			rect_row_size = device_data_size / row_count;
		std::vector<cl::Event>	events;

		for(size_t dev_n = 0; dev_n != device_count; ++dev_n)
		{
			data::devctx&	ctx(d->m_devices[dev_n]);
			size_t			offset = dev_n * rect_row_size;
			cl::Event		ev;
			cl::array<size_t, 3>	buffer_offset{offset, 0, 0};
			cl::array<size_t, 3>	host_offset{offset, 0, 0};
			cl::array<size_t, 3>	region{rect_row_size, row_count, 1};

			ctx.m_queue.enqueueReadBufferRect(f->buffer(dev_n), CL_FALSE, buffer_offset, host_offset,
											  region, row_size, 0, row_size, 0,
											  host_buffer.data(), NULL, &ev);
			events.push_back(ev);
		}
		cl::Event::waitForEvents(events);
		write_buffer(f, host_buffer.data());
	}
}

nbody_engine::memory* nbody_engine_opencl::create_buffer(size_t s)
{
	if(d->m_devices.empty())
	{
		qDebug() << "No OpenCL device available";
		return NULL;
	}
	return new smemory(s, d->m_devices);
}

void nbody_engine_opencl::free_buffer(memory* m)
{
	if(d->m_devices.empty())
	{
		qDebug() << "No OpenCL device available";
		return;
	}
	delete m;
}

// Gather host memory from parts at devices
void nbody_engine_opencl::read_buffer(void* _dst, const memory* _src)
{
	const smemory*	src = dynamic_cast<const smemory*>(_src);

	if(src == NULL)
	{
		qDebug() << "src is not smemory";
		return;
	}

	size_t					device_count(d->m_devices.size());
	size_t					device_data_size = src->size() / device_count;
	std::vector<cl::Event>	events;

	for(size_t dev_n = 0; dev_n != device_count; ++dev_n)
	{
		cl::Event		ev;
		size_t			offset = dev_n * device_data_size;
		data::devctx&	ctx(d->m_devices[dev_n]);
		ctx.m_queue.enqueueReadBuffer(src->buffer(dev_n), CL_FALSE, offset, device_data_size,
									  static_cast<char*>(_dst) + offset, NULL, &ev);
		events.push_back(ev);
	}

	cl::Event::waitForEvents(events);
}

// Copy all host memory to each device
void nbody_engine_opencl::write_buffer(memory* _dst, const void* _src)
{
	smemory*	dst = dynamic_cast<smemory*>(_dst);

	if(dst == NULL)
	{
		qDebug() << "dst is not smemory";
		return;
	}

	size_t					device_count(d->m_devices.size());
	std::vector<cl::Event>	events;

	for(size_t dev_n = 0; dev_n != device_count; ++dev_n)
	{
		cl::Event		ev;
		data::devctx&	ctx(d->m_devices[dev_n]);
		ctx.m_queue.enqueueWriteBuffer(dst->buffer(dev_n), CL_FALSE, 0, dst->size(),
									   static_cast<const char*>(_src), NULL, &ev);
		events.push_back(ev);
	}

	cl::Event::waitForEvents(events);
}

void nbody_engine_opencl::copy_buffer(memory* _a, const memory* _b)
{
	smemory*		a = dynamic_cast<smemory*>(_a);
	const smemory*	b = dynamic_cast<const smemory*>(_b);

	if(a == NULL)
	{
		qDebug() << "a is not smemory";
		return;
	}
	if(b == NULL)
	{
		qDebug() << "b is not smemory";
		return;
	}
	if(a->size() != b->size())
	{
		qDebug() << "Size does not match";
		return;
	}

	QByteArray	host_buffer(static_cast<int>(a->size()), Qt::Uninitialized);

	read_buffer(host_buffer.data(), b);
	write_buffer(a, host_buffer.data());
}

void nbody_engine_opencl::fill_buffer(memory* _a, const nbcoord_t& value)
{
	smemory*		a = dynamic_cast<smemory*>(_a);

	if(a == NULL)
	{
		qDebug() << "a is not smemory";
		return;
	}

	size_t					device_count(d->m_devices.size());
	cl::NDRange				global_range(a->alloc_size() / sizeof(nbcoord_t));
	cl::NDRange				local_range(NBODY_DATA_BLOCK_SIZE);
	std::vector<cl::Event>	events;

	for(size_t dev_n = 0; dev_n != device_count; ++dev_n)
	{
		data::devctx&	ctx(d->m_devices[dev_n]);
		cl::EnqueueArgs	eargs(ctx.m_queue, global_range, local_range);
		cl::Event		ev(ctx.m_fill(eargs, a->buffer(dev_n), value));
		events.push_back(ev);
	}

	cl::Event::waitForEvents(events);
}

void nbody_engine_opencl::fmadd_inplace(memory* _a, const memory* _b, const nbcoord_t& c)
{
	smemory*		a = dynamic_cast<smemory*>(_a);
	const smemory*	b = dynamic_cast<const smemory*>(_b);

	if(a == NULL)
	{
		qDebug() << "a is not smemory";
		return;
	}
	if(b == NULL)
	{
		qDebug() << "b is not smemory";
		return;
	}

	size_t					device_count(d->m_devices.size());
	size_t					device_data_size = problem_size() / device_count;
	cl::NDRange				global_range(device_data_size);
	cl::NDRange				local_range(NBODY_DATA_BLOCK_SIZE);
	std::vector<cl::Event>	events;

	for(size_t dev_n = 0; dev_n != device_count; ++dev_n)
	{
		size_t			offset = dev_n * device_data_size;
		data::devctx&	ctx(d->m_devices[dev_n]);
		cl::EnqueueArgs	eargs(ctx.m_queue, global_range, local_range);
		cl::Event		ev(ctx.m_fmadd1(eargs, offset, a->buffer(dev_n),
										b->buffer(dev_n), c));
		events.push_back(ev);
	}

	cl::Event::waitForEvents(events);
}

void nbody_engine_opencl::fmadd(memory* _a, const memory* _b, const memory* _c, const nbcoord_t& _d)
{
	smemory*		a = dynamic_cast<smemory*>(_a);
	const smemory*	b = dynamic_cast<const smemory*>(_b);
	const smemory*	c = dynamic_cast<const smemory*>(_c);

	if(a == NULL)
	{
		qDebug() << "a is not smemory";
		return;
	}
	if(b == NULL)
	{
		qDebug() << "b is not smemory";
		return;
	}
	if(c == NULL)
	{
		qDebug() << "c is not smemory";
		return;
	}

	size_t					device_count(d->m_devices.size());
	size_t					device_data_size = problem_size() / device_count;
	cl::NDRange				global_range(device_data_size);
	cl::NDRange				local_range(NBODY_DATA_BLOCK_SIZE);
	std::vector<cl::Event>	events;

	for(size_t dev_n = 0; dev_n != device_count; ++dev_n)
	{
		size_t			offset = dev_n * device_data_size;
		data::devctx&	ctx(d->m_devices[dev_n]);
		cl::EnqueueArgs	eargs(ctx.m_queue, global_range, local_range);
		cl::Event		ev(ctx.m_fmadd2(eargs, a->buffer(dev_n), b->buffer(dev_n), c->buffer(dev_n),
										_d, offset, offset, offset));
		events.push_back(ev);
	}

	cl::Event::waitForEvents(events);
}

void nbody_engine_opencl::fmaxabs(const memory* _a, nbcoord_t& result)
{
	const smemory*	a = dynamic_cast<const smemory*>(_a);

	if(a == NULL)
	{
		qDebug() << "a is not smemory";
		return;
	}

	size_t					rsize = problem_size() / NBODY_DATA_BLOCK_SIZE;
	size_t					device_count(d->m_devices.size());
	size_t					device_data_size = problem_size() / device_count;
	size_t					rdevice_data_size = rsize / device_count;
	cl::NDRange				global_range(rsize);
	cl::NDRange				local_range(4);
	std::vector<cl::Event>	events;
	smemory					out(sizeof(nbcoord_t)*rsize, d->m_devices);

	for(size_t dev_n = 0; dev_n != device_count; ++dev_n)
	{
		size_t			offset = dev_n * device_data_size;
		size_t			roffset = dev_n * rdevice_data_size;
		data::devctx&	ctx(d->m_devices[dev_n]);
		cl::EnqueueArgs	eargs(ctx.m_queue, global_range, local_range);
		cl::Event		ev(ctx.m_fmaxabs(eargs, a->buffer(dev_n), offset,
										 offset + device_data_size,
										 out.buffer(dev_n), roffset));
		events.push_back(ev);
	}

	cl::Event::waitForEvents(events);

	std::vector<nbcoord_t> host_buff(rsize);

	read_buffer(host_buff.data(), &out);

	result = *std::max_element(host_buff.begin(), host_buff.end());
}

void nbody_engine_opencl::print_info() const
{
	std::vector<cl::Platform>		platforms;
	try
	{
		cl::Platform::get(&platforms);
	}
	catch(cl::Error& e)
	{
		qDebug() << e.err() << e.what();
		return;
	}
	qDebug() << "Available platforms & devices:";
	for(size_t i = 0; i != platforms.size(); ++i)
	{
		const cl::Platform&			platform(platforms[i]);
		std::vector<cl::Device>		devices;

		qDebug() << i << platform.getInfo<CL_PLATFORM_VENDOR>().c_str();

		platform.getDevices(CL_DEVICE_TYPE_ALL, &devices);

		cl::Context	context(devices);

		for(size_t j = 0; j != devices.size(); ++j)
		{
			cl::Device&		device(devices[j]);
			qDebug() << "\t--device=" << QString("%1:%2").arg(i).arg(j);
			qDebug() << "\t\t CL_DEVICE_NAME" << device.getInfo<CL_DEVICE_NAME>().c_str();
			qDebug() << "\t\t CL_DEVICE_TYPE" << device.getInfo<CL_DEVICE_TYPE>();
			qDebug() << "\t\t CL_DRIVER_VERSION" << device.getInfo<CL_DRIVER_VERSION>().c_str();
			qDebug() << "\t\t CL_DEVICE_PROFILE" << device.getInfo<CL_DEVICE_PROFILE>().c_str();
			qDebug() << "\t\t CL_DEVICE_VERSION" << device.getInfo<CL_DEVICE_VERSION>().c_str();
			qDebug() << "\t\t CL_DEVICE_MAX_COMPUTE_UNITS" << device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>();
			qDebug() << "\t\t CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS" << device.getInfo<CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS>();
			qDebug() << "\t\t CL_DEVICE_MAX_WORK_GROUP_SIZE" << device.getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>();
			qDebug() << "\t\t CL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT" << device.getInfo<CL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT>();
			qDebug() << "\t\t CL_DEVICE_NATIVE_VECTOR_WIDTH_FLOAT" << device.getInfo<CL_DEVICE_NATIVE_VECTOR_WIDTH_FLOAT>();
			qDebug() << "\t\t CL_DEVICE_PREFERRED_VECTOR_WIDTH_DOUBLE" << device.getInfo<CL_DEVICE_PREFERRED_VECTOR_WIDTH_DOUBLE>();
			qDebug() << "\t\t CL_DEVICE_NATIVE_VECTOR_WIDTH_DOUBLE" << device.getInfo<CL_DEVICE_NATIVE_VECTOR_WIDTH_DOUBLE>();
//			qDebug() << "\t\t CL_DEVICE_GLOBAL_MEM_CACHE_TYPE" << device.getInfo<CL_DEVICE_GLOBAL_MEM_CACHE_TYPE>().c_str();
			qDebug() << "\t\t CL_DEVICE_GLOBAL_MEM_CACHELINE_SIZE" << device.getInfo<CL_DEVICE_GLOBAL_MEM_CACHELINE_SIZE>();
			qDebug() << "\t\t CL_DEVICE_GLOBAL_MEM_CACHE_SIZE" << device.getInfo<CL_DEVICE_GLOBAL_MEM_CACHE_SIZE>();
			qDebug() << "\t\t CL_DEVICE_GLOBAL_MEM_SIZE" << device.getInfo<CL_DEVICE_GLOBAL_MEM_SIZE>();
			qDebug() << "\t\t CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE" << device.getInfo<CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE>();
			qDebug() << "\t\t CL_DEVICE_MAX_CONSTANT_ARGS" << device.getInfo<CL_DEVICE_MAX_CONSTANT_ARGS>();
			qDebug() << "\t\t CL_DEVICE_LOCAL_MEM_TYPE" << device.getInfo<CL_DEVICE_LOCAL_MEM_TYPE>();
			qDebug() << "\t\t CL_DEVICE_LOCAL_MEM_SIZE" << device.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>();

			try
			{
				nbody_engine_opencl::data::devctx	c(context, device);
			}
			catch(cl::Error& e)
			{
				qDebug() << e.err() << e.what();
			}
		}
	}
	return;
}

int nbody_engine_opencl::select_devices(const QString& devices)
{
	return d->select_devices(devices);
}
