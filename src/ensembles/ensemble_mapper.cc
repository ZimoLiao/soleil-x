/* Copyright 2017 Stanford University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ensemble_mapper.h"
#include "soleil_types.h"
#include "default_mapper.h"
#include "json.h"

#include "two_shelves.inl"

double cost_fn_linear(const Config &config, unsigned num_procs)
{
  static double cutoff = 32 * 32 * 32;
  long grid_size = (long)config.grid.xNum * config.grid.yNum * config.grid.zNum;
  double cost = std::max(cutoff, grid_size / (1.8 * num_procs));
  return cost * config.integrator.maxIter;
}

double cost_fn_step(const Config &config, unsigned num_procs)
{
  static double cutoff = 32 * 32 * 32;
  long grid_size = (long)config.grid.xNum * config.grid.yNum * config.grid.zNum;
  double cost = std::max(cutoff, grid_size / (1.8 * exp2((int)log2(num_procs))));
  return cost * config.integrator.maxIter;
}

double cost_fn_heuristic(const Config &config, unsigned num_procs)
{
  static double increment[5] = {1, 1.67, 1.78, 1.95, 1.83};
  static double cutoff = 32 * 64 * 64;
  num_procs = exp2((int)log2(num_procs));
  long grid_size =
    (long)config.grid.xNum * config.grid.yNum * config.grid.zNum / num_procs;
  int steps = std::max(1, (int)log2((double)grid_size / cutoff)) - 1;
  double cost = cutoff;
  for (int i = 0; i < std::min(steps, 5); ++i) cost *= increment[i];
  cost *= exp2(std::max(0, steps - 4));
  return cost;
}

using namespace Legion;
using namespace Legion::Mapping;

static LegionRuntime::Logger::Category log_ensemble("ensemble");

class EnsembleMapper : public DefaultMapper
{
public:
  EnsembleMapper(MapperRuntime *rt, Machine machine, Processor local,
                const char *mapper_name,
                std::vector<Processor>* loc_procs_list,
                std::vector<Processor>* omp_procs_list,
                std::vector<Processor>* io_procs_list,
                std::vector<Memory>* sysmems_list,
                std::map<Memory, std::vector<Processor> >* sysmem_local_procs,
                std::map<Memory, std::vector<Processor> >* sysmem_local_io_procs,
                std::map<Processor, Memory>* proc_sysmems,
                std::map<Processor, Memory>* proc_regmems,
                std::map<Processor, unsigned>* proc_ids,
                std::vector<Config>* configs,
                std::vector<TaskMapping>* mappings);
  bool default_policy_select_must_epoch_processors(
      MapperContext ctx,
      const std::vector<std::set<const Task *> > &tasks,
      Processor::Kind proc_kind,
      std::map<const Task *, Processor> &target_procs);
  virtual void select_task_options(const MapperContext    ctx,
                                   const Task&            task,
                                         TaskOptions&     output);
  virtual void default_policy_rank_processor_kinds(
                                    MapperContext ctx, const Task &task,
                                    std::vector<Processor::Kind> &ranking);
  virtual Processor default_policy_select_initial_processor(
                                    MapperContext ctx, const Task &task);
  virtual void default_policy_select_target_processors(
                                    MapperContext ctx,
                                    const Task &task,
                                    std::vector<Processor> &target_procs);
  virtual Memory default_policy_select_target_memory(MapperContext ctx,
                                    Processor target_proc,
                                    const RegionRequirement &req);
  virtual LogicalRegion default_policy_select_instance_region(
                                MapperContext ctx, Memory target_memory,
                                const RegionRequirement &req,
                                const LayoutConstraintSet &constraints,
                                bool force_new_instances,
                                bool meets_constraints);
  virtual void map_task(const MapperContext      ctx,
                        const Task&              task,
                        const MapTaskInput&      input,
                              MapTaskOutput&     output);
  virtual void map_copy(const MapperContext ctx,
                        const Copy &copy,
                        const MapCopyInput &input,
                        MapCopyOutput &output);
  virtual void slice_task(const MapperContext      ctx,
                          const Task&              task,
                          const SliceTaskInput&    input,
                                SliceTaskOutput&   output);
  virtual void select_tunable_value(const MapperContext         ctx,
                                    const Task&                 task,
                                    const SelectTunableInput&   input,
                                          SelectTunableOutput&  output);
protected:
  template<bool IS_SRC>
  void ensemble_create_copy_instance(MapperContext ctx, const Copy &copy,
                                   const RegionRequirement &req, unsigned index,
                                   std::vector<PhysicalInstance> &instances);
private:
  std::vector<Processor>& loc_procs_list;
  std::vector<Processor>& omp_procs_list;
  std::vector<Processor>& io_procs_list;
  std::vector<Memory>& sysmems_list;
  std::map<Memory, std::vector<Processor> >& sysmem_local_procs;
  std::map<Memory, std::vector<Processor> >& sysmem_local_io_procs;
  std::map<Processor, Memory>& proc_sysmems;
  std::map<Processor, Memory>& proc_regmems;
  std::map<Processor, unsigned>& proc_ids;
  std::vector<Config>& configs;
  std::vector<TaskMapping>& mappings;
  typedef std::pair<UniqueID, Domain> SliceCacheKey;
  typedef std::vector<TaskSlice> SliceCache;
  std::map<SliceCacheKey, SliceCache> slice_caches;
  TaskID work_task_id;
  std::set<TaskID> sweep_tasks;
  typedef std::pair<UniqueID, LogicalRegion> SweepCacheKey;
  std::map<SweepCacheKey, Processor> sweep_caches;
  std::set<TaskID> bound_tasks;
  std::map<unsigned, unsigned> last_bound_task;
};

EnsembleMapper::EnsembleMapper(MapperRuntime *rt, Machine machine, Processor local,
                               const char *mapper_name,
                               std::vector<Processor>* _loc_procs_list,
                               std::vector<Processor>* _omp_procs_list,
                               std::vector<Processor>* _io_procs_list,
                               std::vector<Memory>* _sysmems_list,
                               std::map<Memory, std::vector<Processor> >* _sysmem_local_procs,
                               std::map<Memory, std::vector<Processor> >* _sysmem_local_io_procs,
                               std::map<Processor, Memory>* _proc_sysmems,
                               std::map<Processor, Memory>* _proc_regmems,
                               std::map<Processor, unsigned>* _proc_ids,
                               std::vector<Config>* _configs,
                               std::vector<TaskMapping>* _mappings)
  : DefaultMapper(rt, machine, local, mapper_name),
    loc_procs_list(*_loc_procs_list),
    omp_procs_list(*_omp_procs_list),
    io_procs_list(*_io_procs_list),
    sysmems_list(*_sysmems_list),
    sysmem_local_procs(*_sysmem_local_procs),
    sysmem_local_io_procs(*_sysmem_local_io_procs),
    proc_sysmems(*_proc_sysmems),
    proc_regmems(*_proc_regmems),
    proc_ids(*_proc_ids), configs(*_configs),
    mappings(*_mappings),
    work_task_id(0)
{
}

bool EnsembleMapper::default_policy_select_must_epoch_processors(
    MapperContext ctx,
    const std::vector<std::set<const Task *> > &tasks,
    Processor::Kind proc_kind,
    std::map<const Task *, Processor> &target_procs)
{
  const char *ptr = static_cast<const char*>(
      (*(tasks.begin()->begin()))->parent_task->args);
  const Config *config =
    reinterpret_cast<const Config*>(ptr + sizeof(uint64_t));
  unsigned id = config->unique_id;
  TaskMapping &mapping = mappings[id];

  unsigned idx = mapping.start_idx;
  for (std::vector<std::set<const Task *> >::const_iterator it = tasks.begin();
       it != tasks.end(); ++it)
    for (std::set<const Task *>::const_iterator tit = it->begin();
         tit != it->end(); ++tit)
      if (target_procs.find(*tit) == target_procs.end())
        target_procs[*tit] =
          proc_kind == Processor::IO_PROC ? io_procs_list[idx++]
                                          : loc_procs_list[idx++];
  assert(idx == mapping.end_idx + 1);
  return true;
}

void EnsembleMapper::select_task_options(const MapperContext    ctx,
                                         const Task&            task,
                                               TaskOptions&     output)
{
  output.initial_proc = default_policy_select_initial_processor(ctx, task);
  output.inline_task = false;
  output.stealable = stealing_enabled;
  output.map_locally = false;
}

void EnsembleMapper::default_policy_rank_processor_kinds(MapperContext ctx,
                        const Task &task, std::vector<Processor::Kind> &ranking)
{
  const char* task_name = task.get_task_name();
  const char* prefix = "shard_";
  if (strncmp(task_name, prefix, strlen(prefix)) == 0) {
    // Put shard tasks on IO processors.
    ranking.resize(4);
    ranking[0] = Processor::TOC_PROC;
    ranking[1] = Processor::PROC_SET;
    ranking[2] = Processor::IO_PROC;
    ranking[3] = Processor::LOC_PROC;
  } else {
    DefaultMapper::default_policy_rank_processor_kinds(ctx, task, ranking);
  }
}

Processor EnsembleMapper::default_policy_select_initial_processor(
                                    MapperContext ctx, const Task &task)
{
  const char *task_name = task.get_task_name();
  if (task.task_id == work_task_id ||
      (work_task_id == 0 && strcmp(task_name, "work") == 0))
  {
    work_task_id = task.task_id;
    const char *ptr = static_cast<const char*>(task.args);
    const Config *config =
      reinterpret_cast<const Config*>(ptr + sizeof(uint64_t));
    unsigned id = config->unique_id;
    return loc_procs_list[mappings[id].main_idx];
  }
  //else if (sweep_tasks.find(task.task_id) != sweep_tasks.end() ||
  //         (sweep_tasks.size() < 8 &&
  //          strncmp(task_name, "sweep", 5) == 0))
  //{
  //  LogicalRegion region = task.regions[0].region;
  //  SweepCacheKey key(task.parent_task->get_unique_id(), region);
  //  std::map<SweepCacheKey, Processor>::iterator finder =
  //    sweep_caches.find(key);
  //  if (finder != sweep_caches.end()) return finder->second;

  //  sweep_tasks.insert(task.task_id);
  //  const char *ptr = static_cast<const char*>(task.parent_task->args);
  //  const Config *config =
  //    reinterpret_cast<const Config*>(ptr + sizeof(uint64_t));
  //  unsigned id = config->unique_id;
  //  TaskMapping &mapping = mappings[id];

  //  IndexPartition ip =
  //    runtime->get_parent_index_partition(ctx, region.get_index_space());
  //  Domain domain = runtime->get_index_partition_color_space(ctx, ip);
  //  DomainPoint point =
  //    runtime->get_logical_region_color_point(ctx, region);
  //  coord_t size_x = domain.rect_data[3] - domain.rect_data[0] + 1;
  //  coord_t size_y = domain.rect_data[4] - domain.rect_data[1] + 1;
  //  Color color = point.point_data[0] +
  //                point.point_data[1] * size_x +
  //                point.point_data[2] * size_x * size_y;
  //  assert(mapping.start_idx + color <= mapping.end_idx);
  //  assert(mapping.start_idx + color < loc_procs_list.size());
  //  Processor target_proc = loc_procs_list[mapping.start_idx + color];
  //  sweep_caches[key] = target_proc;
  //  return target_proc;
  //}
  //else if (bound_tasks.find(task.task_id) != bound_tasks.end() ||
  //         (bound_tasks.size() < 6 &&
  //          (strcmp(task_name, "west_bound") == 0 ||
  //           strcmp(task_name, "east_bound") == 0 ||
  //           strcmp(task_name, "north_bound") == 0 ||
  //           strcmp(task_name, "south_bound") == 0 ||
  //           strcmp(task_name, "up_bound") == 0 ||
  //           strcmp(task_name, "down_bound") == 0)))
  //{
  //  bound_tasks.insert(task.task_id);
  //  const char *ptr = static_cast<const char*>(task.parent_task->args);
  //  const Config *config =
  //    reinterpret_cast<const Config*>(ptr + sizeof(uint64_t));
  //  unsigned id = config->unique_id;
  //  TaskMapping &mapping = mappings[id];
  //  std::map<unsigned, unsigned>::iterator finder = last_bound_task.find(id);
  //  Processor target_proc;
  //  if (finder == last_bound_task.end())
  //  {
  //    target_proc = loc_procs_list[mapping.main_idx];
  //    last_bound_task[id] = mapping.main_idx;
  //  }
  //  else
  //  {
  //    unsigned next_idx = finder->second;
  //    if (++next_idx > mapping.end_idx)
  //      next_idx = mapping.start_idx;
  //    target_proc = loc_procs_list[next_idx];
  //    last_bound_task[id] = next_idx;
  //  }
  //  return target_proc;
  //}
  //else if (task.parent_task != 0)
  //  return loc_procs_list[proc_ids[task.parent_task->target_proc]];
  return DefaultMapper::default_policy_select_initial_processor(ctx, task);
}

void EnsembleMapper::default_policy_select_target_processors(
                                    MapperContext ctx,
                                    const Task &task,
                                    std::vector<Processor> &target_procs)
{
  target_procs.push_back(task.target_proc);
  //if (task.task_id != work_task_id)
  //{
  //  const std::vector<Processor> &local_procs =
  //    sysmem_local_procs[proc_sysmems[task.target_proc]];
  //  target_procs.insert(target_procs.end(), local_procs.begin(),
  //      local_procs.end());
  //}
}

static bool is_ghost(MapperRuntime *runtime,
                     const MapperContext ctx,
                     LogicalRegion leaf)
{
  // If the region has no parent then it was from a duplicated
  // partition and therefore must be a ghost.
  if (!runtime->has_parent_logical_partition(ctx, leaf)) {
    return true;
  }

  return false;
}

Memory EnsembleMapper::default_policy_select_target_memory(MapperContext ctx,
                                                   Processor target_proc,
                                                   const RegionRequirement &req)
{
  Memory target_memory = proc_sysmems[target_proc];
  if (is_ghost(runtime, ctx, req.region)) {
    std::map<Processor, Memory>::iterator finder = proc_regmems.find(target_proc);
    if (finder != proc_regmems.end()) target_memory = finder->second;
  }
  return target_memory;
}

LogicalRegion EnsembleMapper::default_policy_select_instance_region(
                              MapperContext ctx, Memory target_memory,
                              const RegionRequirement &req,
                              const LayoutConstraintSet &constraints,
                              bool force_new_instances,
                              bool meets_constraints)
{
  return req.region;
}

void EnsembleMapper::map_task(const MapperContext      ctx,
                            const Task&              task,
                            const MapTaskInput&      input,
                                  MapTaskOutput&     output)
{
  if (task.parent_task != NULL && task.parent_task->must_epoch_task) {
    Processor::Kind target_kind = task.target_proc.kind();
    // Get the variant that we are going to use to map this task
    VariantInfo chosen = default_find_preferred_variant(task, ctx,
                                                        true/*needs tight bound*/, true/*cache*/, target_kind);
    output.chosen_variant = chosen.variant;
    // TODO: some criticality analysis to assign priorities
    output.task_priority = 0;
    output.postmap_task = false;
    // Figure out our target processors
    output.target_procs.push_back(task.target_proc);

    for (unsigned idx = 0; idx < task.regions.size(); idx++) {
      const RegionRequirement &req = task.regions[idx];

      // Skip any empty regions
      if ((req.privilege == NO_ACCESS) || (req.privilege_fields.empty()))
        continue;

      assert(input.valid_instances[idx].size() > 0);
      output.chosen_instances[idx] = input.valid_instances[idx];
      bool ok = runtime->acquire_and_filter_instances(ctx, output.chosen_instances);
      if (!ok) {
        log_ensemble.error("failed to acquire instances");
        assert(false);
      }
    }
    return;
  }

  DefaultMapper::map_task(ctx, task, input, output);
}

void EnsembleMapper::map_copy(const MapperContext ctx,
                             const Copy &copy,
                             const MapCopyInput &input,
                             MapCopyOutput &output)
{
  log_ensemble.spew("Soleil mapper map_copy");
  for (unsigned idx = 0; idx < copy.src_requirements.size(); idx++)
  {
    // Always use a virtual instance for the source.
    output.src_instances[idx].clear();
    output.src_instances[idx].push_back(
      PhysicalInstance::get_virtual_instance());

    // Place the destination instance on the remote node.
    output.dst_instances[idx].clear();
    if (!copy.dst_requirements[idx].is_restricted()) {
      // Call a customized method to create an instance on the desired node.
      ensemble_create_copy_instance<false/*is src*/>(ctx, copy,
        copy.dst_requirements[idx], idx, output.dst_instances[idx]);
    } else {
      // If it's restricted, just take the instance. This will only
      // happen inside the shard task.
      output.dst_instances[idx] = input.dst_instances[idx];
      if (!output.dst_instances[idx].empty())
        runtime->acquire_and_filter_instances(ctx,
                                output.dst_instances[idx]);
    }
  }
}

template<bool IS_SRC>
void EnsembleMapper::ensemble_create_copy_instance(MapperContext ctx,
                     const Copy &copy, const RegionRequirement &req,
                     unsigned idx, std::vector<PhysicalInstance> &instances)
{
  // This method is identical to the default version except that it
  // chooses an intelligent memory based on the destination of the
  // copy.

  // See if we have all the fields covered
  std::set<FieldID> missing_fields = req.privilege_fields;
  for (std::vector<PhysicalInstance>::const_iterator it =
        instances.begin(); it != instances.end(); it++)
  {
    it->remove_space_fields(missing_fields);
    if (missing_fields.empty())
      break;
  }
  if (missing_fields.empty())
    return;
  // If we still have fields, we need to make an instance
  // We clearly need to take a guess, let's see if we can find
  // one of our instances to use.


  const LogicalRegion& region = copy.src_requirements[idx].region;
  IndexPartition ip = runtime->get_parent_index_partition(ctx, region.get_index_space());
  Domain domain = runtime->get_index_partition_color_space(ctx, ip);
  DomainPoint point =
    runtime->get_logical_region_color_point(ctx, region);
  coord_t size_x = domain.rect_data[3] - domain.rect_data[0] + 1;
  coord_t size_y = domain.rect_data[4] - domain.rect_data[1] + 1;
  Color color = point.point_data[0] +
                point.point_data[1] * size_x +
                point.point_data[2] * size_x * size_y;
  Processor proc = Processor::NO_PROC;
  //if (use_gpu) {
  //  proc = toc_procs_list[color % toc_procs_list.size()];
  //} else if (use_omp) {
  //  proc = omp_procs_list[color % omp_procs_list.size()];
  //} else {
    proc = loc_procs_list[color % loc_procs_list.size()];
  //}
  Memory target_memory = default_policy_select_target_memory(ctx, proc, req);

  bool force_new_instances = false;
  LayoutConstraintSet creation_constraints;
  default_policy_select_constraints(ctx, creation_constraints, target_memory, req);
  creation_constraints.add_constraint(
      FieldConstraint(missing_fields,
                      false/*contig*/, false/*inorder*/));
  instances.resize(instances.size() + 1);
  if (!default_make_instance(ctx, target_memory,
        creation_constraints, instances.back(),
        COPY_MAPPING, force_new_instances, true/*meets*/, req))
  {
    // If we failed to make it that is bad
    log_ensemble.error("Soleil mapper failed allocation for "
                     "%s region requirement %d of explicit "
                     "region-to-region copy operation in task %s "
                     "(ID %lld) in memory " IDFMT " for processor "
                     IDFMT ". This means the working set of your "
                     "application is too big for the allotted "
                     "capacity of the given memory under the default "
                     "mapper's mapping scheme. You have three "
                     "choices: ask Realm to allocate more memory, "
                     "write a custom mapper to better manage working "
                     "sets, or find a bigger machine. Good luck!",
                     IS_SRC ? "source" : "destination", idx,
                     copy.parent_task->get_task_name(),
                     copy.parent_task->get_unique_id(),
		                 target_memory.id,
		                 copy.parent_task->current_proc.id);
    assert(false);
  }
}

void EnsembleMapper::slice_task(const MapperContext      ctx,
                                const Task&              task,
                                const SliceTaskInput&    input,
                                      SliceTaskOutput&   output)
{
  if (task.parent_task == NULL ||
      strcmp(task.parent_task->get_task_name(), "work") != 0)
  {
    DefaultMapper::slice_task(ctx, task, input, output);
    return;
  }


  output.verify_correctness = false;

  SliceCacheKey key(task.parent_task->get_unique_id(), input.domain);
  std::map<SliceCacheKey, SliceCache>::iterator finder =
    slice_caches.find(key);
  if (finder != slice_caches.end())
  {
    output.slices = finder->second;
    return;
  }

  const char *ptr = static_cast<const char*>(task.parent_task->args);
  const Config *config =
    reinterpret_cast<const Config*>(ptr + sizeof(uint64_t));
  TaskMapping &mapping = mappings[config->unique_id];
  std::vector<Processor> procs;
  for (unsigned i = mapping.start_idx; i <= mapping.end_idx; ++i)
    procs.push_back(loc_procs_list[i]);
  assert(mapping.start_idx >= 0);
  assert(mapping.start_idx < loc_procs_list.size());
  assert(mapping.end_idx >= 0);
  assert(mapping.end_idx < loc_procs_list.size());
  DomainT<3,coord_t> point_space = input.domain;
  Point<3,coord_t> num_blocks =
    default_select_num_blocks<3>(procs.size(), point_space.bounds);
  default_decompose_points<3>(point_space, procs,
      num_blocks, false/*recurse*/, false, output.slices);
  slice_caches[key] = output.slices;
}

void EnsembleMapper::select_tunable_value(const MapperContext         ctx,
                                          const Task&                 task,
                                          const SelectTunableInput&   input,
                                                SelectTunableOutput&  output)
{
  if (input.tunable_id == TUNABLE_CONFIG && input.mapping_tag == 0)
  {
    output.size = configs.size() * sizeof(Config) + sizeof(unsigned);
    output.value = malloc(output.size);
    char *buffer = static_cast<char*>(output.value);
    *reinterpret_cast<unsigned*>(buffer) = static_cast<unsigned>(configs.size());
    buffer += sizeof(unsigned);
    for (unsigned idx = 0; idx < configs.size(); ++idx)
    {
      memcpy(buffer, &configs[idx], sizeof(Config));
      buffer += sizeof(Config);
    }
  }
  else
    DefaultMapper::select_tunable_value(ctx, task, input, output);
}

void parse_input_file(const std::string &input_filename,
                      std::vector<std::string> &filenames)
{
  FILE *f = fopen(input_filename.c_str(), "r");
  if (f == NULL)
  {
    printf("Cannot open input file %s\n", input_filename.c_str());
    exit(1);
  }
  int num_configs = -1;
  assert(fscanf(f, "%d\n", &num_configs) == 1);
  assert(num_configs != -1);
  filenames.reserve(num_configs);
  for (int i = 0; i < num_configs; ++i)
  {
    char filename[512];
    assert(fscanf(f, "%s\n", filename) == 1);
    filenames.push_back(filename);
  }
}

enum RadiationType
{
  ALGEBRAIC = 0,
  DOM,
};

Config parse_config(const std::string &filename)
{
  Config config;
  assert(filename.size() < 512);
  memcpy(config.filename, filename.c_str(), filename.size() + 1);
  FILE *f = fopen(filename.c_str(), "r");
  if (f == NULL)
  {
    printf("%s\n", "Cannot open config file");
    exit(1);
  }
  int res1 = fseek(f, 0L, 2);
  if (res1 != 0)
  {
    printf("%s\n", "Cannot seek to end of config file");
    exit(1);
  }
  long len = ftell(f);
  if (len < 0L)
  {
    printf("%s\n", "Cannot ftell config file");
    exit(1);
  }
  long res2 = fseek(f, 0L, 0);
  if (res2 != 0)
  {
    printf("%s\n", "Cannot seek to start of config file");
    exit(1);
  }
  char *buf = (char*)malloc((size_t)len);
  if (buf == NULL)
  {
    printf("%s\n", "Malloc error while parsing config");
    exit(1);
  }
  size_t res3 = fread(buf, 1L, (size_t)len, f);
  if (res3 < (size_t)len)
  {
    printf("%s\n", "Cannot read from config file");
    exit(1);
  }
  fclose(f);
  char errMsg[256];
  json_settings settings;
  memset(&settings, 0, sizeof(json_settings));
  json_value *root = json_parse_ex(&settings, buf, (size_t)len, errMsg);
  if (root == NULL)
  {
    printf("JSON parsing error: %s\n", errMsg);
    exit(1);
  }
  int totalParsed = 0;
  if (root->type != 1U)
  {
    printf("%s for option %s\n", "Wrong type", "<root>");
    exit(1);
  }
  for (unsigned i = 0; i < root->u.object.length; ++i)
  {
    json_char *nodeName = root->u.object.values[i].name;
    struct _json_value *nodeValue = root->u.object.values[i].value;
    bool parsed = false;
    if (strcmp(nodeName, "IO") == 0)
    {
      int totalParsedv1 = 0;
      if (nodeValue->type != (unsigned)(1))
      {
        printf("%s for option %s\n", "Wrong type", nodeName);
        exit(1);
      }
      for (unsigned iv1 = 0; iv1 < nodeValue->u.object.length; ++iv1)
      {
        json_char *nodeNamev1 = nodeValue->u.object.values[iv1].name;
        struct _json_value *nodeValuev1 = nodeValue->u.object.values[iv1].value;
        bool parsedv1 = false;
        if (strcmp(nodeNamev1, "headerFrequency") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.io.headerFrequency = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "consoleFrequency") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.io.consoleFrequency = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "restartEveryTimeSteps") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.io.restartEveryTimeSteps = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "wrtRestart") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "OFF") == 0) {
            config.io.wrtRestart = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "ON") == 0) {
            config.io.wrtRestart = 2 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (parsedv1)
          totalParsedv1 = totalParsedv1 + 1;
        //else
        //  printf("Ignoring option %s\n", nodeNamev1);
      }
      if (totalParsedv1 < 4)
      {
        printf("%s\n", "Missing options from config file");
        assert(false);
        exit(1);
      }
      parsed = true;
    }
    if (strcmp(nodeName, "Integrator") == 0)
    {
      int totalParsedv1 = 0;
      if (nodeValue->type != (unsigned)(1))
      {
        printf("%s for option %s\n", "Wrong type", nodeName);
        exit(1);
      }
      for (unsigned iv1 = 0; iv1 < nodeValue->u.object.length; ++iv1)
      {
        json_char *nodeNamev1 = nodeValue->u.object.values[iv1].name;
        struct _json_value *nodeValuev1 = nodeValue->u.object.values[iv1].value;
        bool parsedv1 = false;
        if (strcmp(nodeNamev1, "finalTime") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.integrator.finalTime = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "restartIter") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.integrator.restartIter = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "fixedDeltaTime") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.integrator.fixedDeltaTime = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "cfl") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.integrator.cfl = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "maxIter") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.integrator.maxIter = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (parsedv1)
          totalParsedv1 = totalParsedv1 + 1;
        //else
        //  printf("Ignoring option %s\n", nodeNamev1);
      }
      if (totalParsedv1 < 5)
      {
        printf("%s\n", "Missing options from config file");
        assert(false);
        exit(1);
      }
      parsed = true;
    }
    if (strcmp(nodeName, "Flow") == 0)
    {
      int totalParsedv1 = 0;
      if (nodeValue->type != (unsigned)(1))
      {
        printf("%s for option %s\n", "Wrong type", nodeName);
        exit(1);
      }
      for (unsigned iv1 = 0; iv1 < nodeValue->u.object.length; ++iv1)
      {
        json_char *nodeNamev1 = nodeValue->u.object.values[iv1].name;
        struct _json_value *nodeValuev1 = nodeValue->u.object.values[iv1].value;
        bool parsedv1 = false;
        if (strcmp(nodeNamev1, "initCase") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "Uniform") == 0) {
            config.flow.initCase = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Restart") == 0) {
            config.flow.initCase = 2 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Perturbed") == 0) {
            config.flow.initCase = 3 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "TaylorGreen2DVortex") == 0) {
            config.flow.initCase = 4 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "TaylorGreen3DVortex") == 0) {
            config.flow.initCase = 5 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "powerlawViscRef") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.flow.powerlawViscRef = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "sutherlandTempRef") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.flow.sutherlandTempRef = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "turbForcing") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "OFF") == 0) {
            config.flow.turbForcing = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "ON") == 0) {
            config.flow.turbForcing = 2 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "prandtl") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.flow.prandtl = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "sutherlandSRef") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.flow.sutherlandSRef = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "initParams") == 0) {
          if (nodeValuev1->type != (unsigned)(2)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (nodeValuev1->u.array.length != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong length", nodeNamev1);
            exit(1);
          }
          for (int iv2 = 0; iv2 < 5; ++iv2) {
            struct _json_value *rval_i = nodeValuev1->u.array.values[iv2];
            if (rval_i->type != (unsigned)(4)) {
              printf("%s for option %s\n", "Wrong element type", nodeNamev1);
              exit(1);
            }
            ((double*)config.flow.initParams)[iv2] = rval_i->u.dbl;
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "bodyForce") == 0) {
          if (nodeValuev1->type != (unsigned)(2)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (nodeValuev1->u.array.length != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong length", nodeNamev1);
            exit(1);
          }
          for (int iv2 = 0; iv2 < 3; ++iv2)
          {
            struct _json_value *rval_i = nodeValuev1->u.array.values[iv2];
            if (rval_i->type != (unsigned)(4)) {
              printf("%s for option %s\n", "Wrong element type", nodeNamev1);
              exit(1);
            }
            ((double*)config.flow.bodyForce)[iv2] = rval_i->u.dbl;
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "sutherlandViscRef") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.flow.sutherlandViscRef = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "viscosityModel") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "Constant") == 0) {
            config.flow.viscosityModel = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "PowerLaw") == 0) {
            config.flow.viscosityModel = 2 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Sutherland") == 0) {
            config.flow.viscosityModel = 3 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "powerlawTempRef") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.flow.powerlawTempRef = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "constantVisc") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.flow.constantVisc = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "gasConstant") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.flow.gasConstant = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "gamma") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.flow.gamma = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (parsedv1)
          totalParsedv1 = totalParsedv1 + 1;
        //else
        //  printf("Ignoring option %s\n", nodeNamev1);
      }
      if (totalParsedv1 < 14)
      {
        printf("%s\n", "Missing options from config file");
        assert(false);
        exit(1);
      }
      parsed = true;
    }
    if (strcmp(nodeName, "Particles") == 0)
    {
      int totalParsedv1 = 0;
      if (nodeValue->type != (unsigned)(1))
      {
        printf("%s for option %s\n", "Wrong type", nodeName);
        exit(1);
      }
      for (unsigned iv1 = 0; iv1 < nodeValue->u.object.length; ++iv1)
      {
        json_char *nodeNamev1 = nodeValue->u.object.values[iv1].name;
        struct _json_value *nodeValuev1 = nodeValue->u.object.values[iv1].value;
        bool parsedv1 = false;
        if (strcmp(nodeNamev1, "convectiveCoeff") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.convectiveCoeff = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "diameterMean") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.diameterMean = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "absorptivity") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.absorptivity = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "restitutionCoeff") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.restitutionCoeff = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "density") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.density = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "maxNum") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.maxNum = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "bodyForce") == 0) {
          if (nodeValuev1->type != (unsigned)(2)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (nodeValuev1->u.array.length != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong length", nodeNamev1);
            exit(1);
          }
          for (int iv2 = 0; iv2 < 3; ++iv2)
          {
            struct _json_value *rval_i = nodeValuev1->u.array.values[iv2];
            if (rval_i->type != (unsigned)(4)) {
              printf("%s for option %s\n", "Wrong element type", nodeNamev1);
              exit(1);
            }
            ((double*)config.particles.bodyForce)[iv2] = rval_i->u.dbl;
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "maxXferNum") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.maxXferNum = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "maxSkew") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.maxSkew = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "initNum") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.initNum = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "initCase") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "Random") == 0) {
            config.particles.initCase = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Restart") == 0) {
            config.particles.initCase = 2 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Uniform") == 0) {
            config.particles.initCase = 3 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "initTemperature") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.initTemperature = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "heatCapacity") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.particles.heatCapacity = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (parsedv1)
          totalParsedv1 = totalParsedv1 + 1;
        //else
        //  printf("Ignoring option %s\n", nodeNamev1);
      }
      if (totalParsedv1 < 13)
      {
        printf("%s\n", "Missing options from config file");
        assert(false);
        exit(1);
      }
      parsed = true;
    }
    if (strcmp(nodeName, "BC") == 0)
    {
      int totalParsedv1 = 0;
      if (nodeValue->type != (unsigned)(1))
      {
        printf("%s for option %s\n", "Wrong type", nodeName);
        exit(1);
      }
      for (unsigned iv1 = 0; iv1 < nodeValue->u.object.length; ++iv1)
      {
        json_char *nodeNamev1 = nodeValue->u.object.values[iv1].name;
        struct _json_value *nodeValuev1 = nodeValue->u.object.values[iv1].value;
        bool parsedv1 = false;
        if (strcmp(nodeNamev1, "xBCLeftVel") == 0) {
          if (nodeValuev1->type != (unsigned)(2)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (nodeValuev1->u.array.length != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong length", nodeNamev1);
            exit(1);
          }
          for (int iv2 = 0; iv2 < 3; ++iv2)
          {
            struct _json_value *rval_i = nodeValuev1->u.array.values[iv2];
            if (rval_i->type != (unsigned)(4)) {
              printf("%s for option %s\n", "Wrong element type", nodeNamev1);
              exit(1);
            }
            ((double*)config.bc.xBCLeftVel)[iv2] = rval_i->u.dbl;
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "yBCLeftVel") == 0) {
          if (nodeValuev1->type != (unsigned)(2)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (nodeValuev1->u.array.length != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong length", nodeNamev1);
            exit(1);
          }
          for (int iv2 = 0; iv2 < 3; ++iv2)
          {
            struct _json_value *rval_i = nodeValuev1->u.array.values[iv2];
            if (rval_i->type != (unsigned)(4)) {
              printf("%s for option %s\n", "Wrong element type", nodeNamev1);
              exit(1);
            }
            ((double*)config.bc.yBCLeftVel)[iv2] = rval_i->u.dbl;
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "xBCRightVel") == 0) {
          if (nodeValuev1->type != (unsigned)(2)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (nodeValuev1->u.array.length != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong length", nodeNamev1);
            exit(1);
          }
          for (int iv2 = 0; iv2 < 3; ++iv2)
          {
            struct _json_value *rval_i = nodeValuev1->u.array.values[iv2];
            if (rval_i->type != (unsigned)(4)) {
              printf("%s for option %s\n", "Wrong element type", nodeNamev1);
              exit(1);
            }
            ((double*)config.bc.xBCRightVel)[iv2] = rval_i->u.dbl;
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "xBCRight") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "Periodic") == 0) {
            config.bc.xBCRight = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Symmetry") == 0) {
            config.bc.xBCRight = 2 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "AdiabaticWall") == 0) {
            config.bc.xBCRight = 3 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "IsothermalWall") == 0) {
            config.bc.xBCRight = 4 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "xBCRightTemp") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.bc.xBCRightTemp = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "zBCRight") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "Periodic") == 0) {
            config.bc.zBCRight = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Symmetry") == 0) {
            config.bc.zBCRight = 2 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "AdiabaticWall") == 0) {
            config.bc.zBCRight = 3 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "IsothermalWall") == 0) {
            config.bc.zBCRight = 4 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "yBCRight") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "Periodic") == 0) {
            config.bc.yBCRight = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Symmetry") == 0) {
            config.bc.yBCRight = 2 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "AdiabaticWall") == 0) {
            config.bc.yBCRight = 3 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "IsothermalWall") == 0) {
            config.bc.yBCRight = 4 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "zBCLeftTemp") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.bc.zBCLeftTemp = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "xBCLeft") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "Periodic") == 0) {
            config.bc.xBCLeft = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Symmetry") == 0) {
            config.bc.xBCLeft = 2 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "AdiabaticWall") == 0) {
            config.bc.xBCLeft = 3 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "IsothermalWall") == 0) {
            config.bc.xBCLeft = 4 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "xBCLeftTemp") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.bc.xBCLeftTemp = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "zBCRightTemp") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.bc.zBCRightTemp = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "yBCLeft") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "Periodic") == 0) {
            config.bc.yBCLeft = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Symmetry") == 0) {
            config.bc.yBCLeft = 2 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "AdiabaticWall") == 0) {
            config.bc.yBCLeft = 3 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "IsothermalWall") == 0) {
            config.bc.yBCLeft = 4 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "yBCRightVel") == 0) {
          if (nodeValuev1->type != (unsigned)(2)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (nodeValuev1->u.array.length != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong length", nodeNamev1);
            exit(1);
          }
          for (int iv2 = 0; iv2 < 3; ++iv2)
          {
            struct _json_value *rval_i = nodeValuev1->u.array.values[iv2];
            if (rval_i->type != (unsigned)(4)) {
              printf("%s for option %s\n", "Wrong element type", nodeNamev1);
              exit(1);
            }
            ((double*)config.bc.yBCRightVel)[iv2] = rval_i->u.dbl;
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "zBCRightVel") == 0) {
          if (nodeValuev1->type != (unsigned)(2)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (nodeValuev1->u.array.length != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong length", nodeNamev1);
            exit(1);
          }
          for (int iv2 = 0; iv2 < 3; ++iv2)
          {
            struct _json_value *rval_i = nodeValuev1->u.array.values[iv2];
            if (rval_i->type != (unsigned)(4)) {
              printf("%s for option %s\n", "Wrong element type", nodeNamev1);
              exit(1);
            }
            ((double*)config.bc.zBCRightVel)[iv2] = rval_i->u.dbl;
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "yBCLeftTemp") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.bc.yBCLeftTemp = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "yBCRightTemp") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.bc.yBCRightTemp = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "zBCLeftVel") == 0) {
          if (nodeValuev1->type != (unsigned)(2)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (nodeValuev1->u.array.length != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong length", nodeNamev1);
            exit(1);
          }
          for (int iv2 = 0; iv2 < 3; ++iv2)
          {
            struct _json_value *rval_i = nodeValuev1->u.array.values[iv2];
            if (rval_i->type != (unsigned)(4)) {
              printf("%s for option %s\n", "Wrong element type", nodeNamev1);
              exit(1);
            }
            ((double*)config.bc.zBCLeftVel)[iv2] = rval_i->u.dbl;
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "zBCLeft") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          bool found = false;
          if (strcmp(nodeValuev1->u.string.ptr, "Periodic") == 0) {
            config.bc.zBCLeft = 1 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Symmetry") == 0) {
            config.bc.zBCLeft = 2 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "AdiabaticWall") == 0) {
            config.bc.zBCLeft = 3 - 1;
            found = true;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "IsothermalWall") == 0) {
            config.bc.zBCLeft = 4 - 1;
            found = true;
          }
          if (not found) {
            printf("%s for option %s\n", "Unexpected value", nodeNamev1);
            exit(1);
          }
          parsedv1 = true;
        }
        if (parsedv1)
          totalParsedv1 = totalParsedv1 + 1;
        //else
        //  printf("Ignoring option %s\n", nodeNamev1);
      }
      if (totalParsedv1 < 18)
      {
        printf("%s\n", "Missing options from config file");
        assert(false);
        exit(1);
      }
      parsed = true;
    }
    if (strcmp(nodeName, "Grid") == 0)
    {
      int totalParsedv1 = 0;
      if (nodeValue->type != (unsigned)(1))
      {
        printf("%s for option %s\n", "Wrong type", nodeName);
        exit(1);
      }
      for (unsigned iv1 = 0; iv1 < nodeValue->u.object.length; ++iv1)
      {
        json_char *nodeNamev1 = nodeValue->u.object.values[iv1].name;
        struct _json_value *nodeValuev1 = nodeValue->u.object.values[iv1].value;
        bool parsedv1 = false;
        if (strcmp(nodeNamev1, "xWidth") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.grid.xWidth = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "xNum") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.grid.xNum = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "yTiles") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.grid.yTiles = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "zTiles") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.grid.zTiles = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "zWidth") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.grid.zWidth = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "origin") == 0) {
          if (nodeValuev1->type != (unsigned)(2)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (nodeValuev1->u.array.length != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong length", nodeNamev1);
            exit(1);
          }
          for (int iv2 = 0; iv2 < 3; ++iv2)
          {
            struct _json_value *rval_i = nodeValuev1->u.array.values[iv2];
            if (rval_i->type != (unsigned)(4)) {
              printf("%s for option %s\n", "Wrong element type", nodeNamev1);
              exit(1);
            }
            ((double*)config.grid.origin)[iv2] = rval_i->u.dbl;
          }
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "yNum") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.grid.yNum = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "yWidth") == 0) {
          if (nodeValuev1->type != (unsigned)(4)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.grid.yWidth = nodeValuev1->u.dbl;
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "xTiles") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.grid.xTiles = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (strcmp(nodeNamev1, "zNum") == 0) {
          if (nodeValuev1->type != (unsigned)(3)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          config.grid.zNum = (int)(nodeValuev1->u.integer);
          parsedv1 = true;
        }
        if (parsedv1)
          totalParsedv1 = totalParsedv1 + 1;
        //else
        //  printf("Ignoring option %s\n", nodeNamev1);
      }
      if (totalParsedv1 < 10)
      {
        printf("%s\n", "Missing options from config file");
        assert(false);
        exit(1);
      }
      parsed = true;
    }
    if (strcmp(nodeName, "Radiation") == 0)
    {
      int totalParsedv1 = 0;
      if (nodeValue->type != (unsigned)(1))
      {
        printf("%s for option %s\n", "Wrong type", nodeName);
        exit(1);
      }
      RadiationType rad_type = ALGEBRAIC;
      bool found_rad_type = false;

      for (unsigned iv1 = 0; iv1 < nodeValue->u.object.length; ++iv1)
      {
        json_char *nodeNamev1 = nodeValue->u.object.values[iv1].name;
        struct _json_value *nodeValuev1 = nodeValue->u.object.values[iv1].value;
        if (strcmp(nodeNamev1, "TYPE") == 0) {
          if (nodeValuev1->type != (unsigned)(5)) {
            printf("%s for option %s\n", "Wrong type", nodeNamev1);
            exit(1);
          }
          if (strcmp(nodeValuev1->u.string.ptr, "Algebraic") == 0) {
            rad_type = ALGEBRAIC;
            found_rad_type = true;
            break;
          }
          if (strcmp(nodeValuev1->u.string.ptr, "DOM") == 0) {
            rad_type = DOM;
            found_rad_type = true;
            break;
          }
          break;
        }
      }
      if (!found_rad_type)
      {
        printf("Missing radiation type\n");
        exit(1);
      }

      switch (rad_type)
      {
        case ALGEBRAIC :
          {
            for (unsigned iv1 = 0; iv1 < nodeValue->u.object.length; ++iv1)
            {
              json_char *nodeNamev1 = nodeValue->u.object.values[iv1].name;
              struct _json_value *nodeValuev1 = nodeValue->u.object.values[iv1].value;
              bool parsedv1 = false;
              if (strcmp(nodeNamev1, "TYPE") == 0) continue;
              if (strcmp(nodeNamev1, "intensity") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.intensity = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (parsedv1)
                totalParsedv1 = totalParsedv1 + 1;
              //else
              //  printf("Ignoring option %s\n", nodeNamev1);
            }
            if (totalParsedv1 < 1)
            {
              printf("%s\n", "Missing options from config file");
              assert(false);
              exit(1);
            }
            parsed = true;
            break;
          }
        case DOM:
          {
            parsed = true;
            for (unsigned iv1 = 0; iv1 < nodeValue->u.object.length; ++iv1)
            {
              json_char *nodeNamev1 = nodeValue->u.object.values[iv1].name;
              struct _json_value *nodeValuev1 = nodeValue->u.object.values[iv1].value;
              bool parsedv1 = false;
              if (strcmp(nodeNamev1, "TYPE") == 0) continue;
              if (strcmp(nodeNamev1, "emissSouth") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.emissSouth = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "tempSouth") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.tempSouth = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "qa") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.qa = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "yNum") == 0) {
                if (nodeValuev1->type != (unsigned)(3)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.yNum = (int)(nodeValuev1->u.integer);
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "tempNorth") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.tempNorth = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "emissWest") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.emissWest = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "emissEast") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.emissEast = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "tempWest") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.tempWest = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "xNum") == 0) {
                if (nodeValuev1->type != (unsigned)(3)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.xNum = (int)(nodeValuev1->u.integer);
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "emissDown") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.emissDown = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "emissUp") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.emissUp = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "tempDown") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.tempDown = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "zNum") == 0) {
                if (nodeValuev1->type != (unsigned)(3)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.zNum = (int)(nodeValuev1->u.integer);
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "emissNorth") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.emissNorth = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "tempEast") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.tempEast = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "qs") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.qs = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (strcmp(nodeNamev1, "tempUp") == 0) {
                if (nodeValuev1->type != (unsigned)(4)) {
                  printf("%s for option %s\n", "Wrong type", nodeNamev1);
                  exit(1);
                }
                config.radiation.tempUp = nodeValuev1->u.dbl;
                parsedv1 = true;
              }
              if (parsedv1)
                totalParsedv1 = totalParsedv1 + 1;
              //else
              //  printf("Ignoring option %s\n", nodeNamev1);
            }
            if (totalParsedv1 < 17)
            {
              printf("%s\n", "Missing options from config file");
              assert(false);
              exit(1);
            }
            parsed = true;
            break;
          }
      }
    }
    if (parsed)
      totalParsed = totalParsed + 1;
    //else
    //  printf("Ignoring option %s\n", nodeName);
  }
  if (totalParsed < 7)
  {
    printf("%s\n", "Missing options from config file");
    assert(false);
    exit(1);
  }
  json_value_free(root);
  free(buf);
  return config;
}

void factorize(Config &config, unsigned dop)
{
  unsigned num_factors = 0;
  unsigned factors[dop];
  while (dop > 1)
  {
    unsigned factor = 2;
    while (factor <= dop)
    {
      if (dop % factor == 0)
      {
        factors[num_factors++] = factor;
        dop /= factor;
        break;
      }
      ++factor;
    }
  }

  int sz_remain[3];
  int extents[3];
  sz_remain[0] = config.grid.xNum;
  sz_remain[1] = config.grid.yNum;
  sz_remain[2] = config.grid.zNum;
  for (int k = 0; k < 3; ++k) extents[k] = 1;
  for (unsigned i = 0; i < num_factors; ++i)
  {
    unsigned factor = factors[i];
    int next_max = 2;
    int max_sz = sz_remain[2];
    for (int k = 1; k >= 0; --k)
      if (max_sz < sz_remain[k])
      {
        next_max = k;
        max_sz = sz_remain[k];
      }
    extents[next_max] *= factor;
    sz_remain[next_max] = (sz_remain[next_max] + factor - 1) / factor;
  }
  config.grid.xTiles = extents[0];
  config.grid.yTiles = extents[1];
  config.grid.zTiles = extents[2];
}

static void create_mappers(Machine machine, HighLevelRuntime *runtime,
                           const std::set<Processor> &local_procs)
{
  InputArgs args = Runtime::get_input_args();
  std::string input_filename;
  bool size_based = false;
  bool size_time_based = false;
  for (int i = 0; i < args.argc; ++i)
    if (strcmp(args.argv[i], "-i") == 0)
      input_filename = args.argv[++i];
    else if (strcmp(args.argv[i], "-size_based") == 0)
      size_based = true;
    else if (strcmp(args.argv[i], "-size_time_based") == 0)
      size_time_based = true;
  if (size_based && size_time_based)
  {
    fprintf(stderr, "You cannot activate both size-based "
        "and size-and-time-based partitionings\n");
    assert(false);
  }
  std::vector<std::string> filenames;
  parse_input_file(input_filename, filenames);
  std::vector<Config> *configs = new std::vector<Config>();
  std::vector<TaskMapping> *mappings = new std::vector<TaskMapping>();
  for (unsigned idx = 0; idx < filenames.size(); ++idx)
  {
    configs->push_back(parse_config(filenames[idx]));
    (*configs)[idx].unique_id = idx;
  }

  std::vector<Processor>* loc_procs_list = new std::vector<Processor>();
  std::vector<Processor>* omp_procs_list = new std::vector<Processor>();
  std::vector<Processor>* io_procs_list = new std::vector<Processor>();
  std::vector<Memory>* sysmems_list = new std::vector<Memory>();
  std::map<Memory, std::vector<Processor> >* sysmem_local_procs =
    new std::map<Memory, std::vector<Processor> >();
  std::map<Memory, std::vector<Processor> >* sysmem_local_io_procs =
    new std::map<Memory, std::vector<Processor> >();
  std::map<Processor, Memory>* proc_sysmems = new std::map<Processor, Memory>();
  std::map<Processor, Memory>* proc_regmems = new std::map<Processor, Memory>();
  std::map<Processor, unsigned>* proc_ids = new std::map<Processor, unsigned>();

  std::vector<Machine::ProcessorMemoryAffinity> proc_mem_affinities;
  machine.get_proc_mem_affinity(proc_mem_affinities);

  for (unsigned idx = 0; idx < proc_mem_affinities.size(); ++idx) {
    Machine::ProcessorMemoryAffinity& affinity = proc_mem_affinities[idx];
    if (affinity.p.kind() == Processor::LOC_PROC ||
        affinity.p.kind() == Processor::IO_PROC ||
        affinity.p.kind() == Processor::OMP_PROC) {
      if (affinity.m.kind() == Memory::SYSTEM_MEM) {
        (*proc_sysmems)[affinity.p] = affinity.m;
      }
      else if (affinity.m.kind() == Memory::REGDMA_MEM) {
        (*proc_regmems)[affinity.p] = affinity.m;
      }
    }
  }

  for (std::map<Processor, Memory>::iterator it = proc_sysmems->begin();
       it != proc_sysmems->end(); ++it) {
    if (it->first.kind() == Processor::LOC_PROC) {
      (*proc_ids)[it->first] = loc_procs_list->size();
      loc_procs_list->push_back(it->first);
      (*sysmem_local_procs)[it->second].push_back(it->first);
    }
    else if (it->first.kind() == Processor::IO_PROC) {
      (*sysmem_local_io_procs)[it->second].push_back(it->first);
      io_procs_list->push_back(it->first);
    }
    else if (it->first.kind() == Processor::OMP_PROC) {
      (*proc_ids)[it->first] = omp_procs_list->size();
      omp_procs_list->push_back(it->first);
    }
  }

  for (std::map<Memory, std::vector<Processor> >::iterator it =
        sysmem_local_procs->begin(); it != sysmem_local_procs->end(); ++it)
    sysmems_list->push_back(it->first);

  if (size_based || size_time_based)
  {
    mappings->resize(configs->size());
    unsigned long total_volume = 0;
    unsigned long total_num_cores = loc_procs_list->size();
    for (unsigned idx = 0; idx < configs->size(); ++idx)
    {
      Config &config = (*configs)[idx];
      total_volume += config.grid.xNum * config.grid.yNum *
        config.grid.zNum * (size_time_based ? config.integrator.maxIter : 1);
    }
    unsigned cores = 0;
    std::set<unsigned> small_tasks;
    for (unsigned idx = 0; idx < configs->size(); ++idx)
    {
      Config &config = (*configs)[idx];
      TaskMapping &mapping = (*mappings)[idx];
      unsigned volume = config.grid.xNum * config.grid.yNum *
        config.grid.zNum * (size_time_based ? config.integrator.maxIter : 1);
      unsigned assigned_cores = total_num_cores * volume / total_volume;
      assigned_cores = (unsigned)round(exp2((int)log2(assigned_cores)));
      if (assigned_cores > 0)
      {
        mapping.main_idx = cores;
        mapping.start_idx = cores;
        mapping.end_idx = cores + assigned_cores - 1;
        assert(mapping.end_idx - mapping.start_idx + 1 == assigned_cores);
        cores += assigned_cores;
      }
      else
        small_tasks.insert(idx);
    }
    unsigned next_core = cores;
    for (std::set<unsigned>::iterator it = small_tasks.begin();
         it != small_tasks.end(); ++it)
    {
      TaskMapping &mapping = (*mappings)[*it];
      if (next_core >= total_num_cores) next_core = cores;
      mapping.main_idx = next_core;
      mapping.start_idx = next_core;
      mapping.end_idx = next_core;
      ++next_core;
    }
  }
  else
  {
    two_shelves<Config, cost_fn_heuristic>(*configs, *mappings,
        loc_procs_list->size() / sysmems_list->size(), sysmems_list->size());
  }
  for (unsigned idx = 0; idx < configs->size(); ++idx)
  {
    TaskMapping &mapping = (*mappings)[idx];
    unsigned dop = mapping.end_idx - mapping.start_idx + 1;
    factorize((*configs)[idx], dop);
  }

  for (std::set<Processor>::const_iterator it = local_procs.begin();
        it != local_procs.end(); it++)
  {
    EnsembleMapper* mapper = new EnsembleMapper(runtime->get_mapper_runtime(),
                                                machine, *it, "ensemble_mapper",
                                                loc_procs_list,
                                                omp_procs_list,
                                                io_procs_list,
                                                sysmems_list,
                                                sysmem_local_procs,
                                                sysmem_local_io_procs,
                                                proc_sysmems,
                                                proc_regmems,
                                                proc_ids,
                                                configs,
                                                mappings);
    runtime->replace_default_mapper(mapper, *it);
  }
}

void register_mappers()
{
  HighLevelRuntime::add_registration_callback(create_mappers);
}
